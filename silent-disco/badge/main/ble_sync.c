#include "ble_sync.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

#include "beacon_packet.h"

static const char *TAG = "ble_sync";

#define MFG_COMPANY_ID 0xFFFF // must match beacon/main/ble_beacon.c

#define SAMPLE_CAP 16
#define WINDOW_US  (5 * 1000 * 1000) // trailing window for the min-offset filter

#define NVS_NAMESPACE       "silentdisco"
#define NVS_KEY_LATENCY_MS  "lat_off_ms"
#define LATENCY_MIN_MS      0
#define LATENCY_MAX_MS      500

typedef struct {
    int64_t offset_us;
    int64_t recv_time_us;
} offset_sample_t;

// Cross-task shared state: written from the NimBLE host task (handle_ext_disc),
// read from the LED-render task. Simple scalars only, volatile, no lock --
// same convention bugdex/main/ble_proto.c uses for its radar fields.
static volatile bool s_have_reference;
static volatile int64_t s_beat_timestamp_us;
static volatile uint8_t s_channel_id;
static volatile uint8_t s_bpm;
static volatile bool s_have_offset;
static volatile int64_t s_offset_estimate_us;
static volatile int32_t s_latency_offset_ms;

static offset_sample_t s_samples[SAMPLE_CAP];
static int s_sample_count;
static int s_next_idx;

// Most recently broadcast pattern lookahead (docs/packet-format.md).
// Written wholesale in handle_ext_disc, read wholesale in
// ble_sync_current_pattern() -- same accepted-risk convention as the
// scalars above, just applied to a small fixed-size array.
static sd_segment_t s_segments[SD_SCHEDULE_LOOKAHEAD];

bool ble_sync_synced(void)
{
    return s_have_offset;
}

uint8_t ble_sync_channel(void)
{
    return s_channel_id;
}

uint8_t ble_sync_bpm(void)
{
    return s_bpm;
}

int64_t ble_sync_phase_us(void)
{
    uint8_t bpm = s_bpm ? s_bpm : 1;
    int64_t period_us = 60000000LL / bpm;
    // Subtracting the latency offset here makes "now" look earlier, which
    // delays the moment phase_us crosses back to 0 by that many ms --
    // i.e. it shifts the beat trigger later in real time, to match a
    // Bluetooth headset's own playback delay (see ble_sync.h).
    int64_t beacon_now_est = esp_timer_get_time() - s_offset_estimate_us - (int64_t)s_latency_offset_ms * 1000;
    int64_t phase = (beacon_now_est - s_beat_timestamp_us) % period_us;
    if (phase < 0) {
        phase += period_us;
    }
    return phase;
}

int64_t ble_sync_beat_index(void)
{
    uint8_t bpm = s_bpm ? s_bpm : 1;
    int64_t period_us = 60000000LL / bpm;
    int64_t beacon_now_est = esp_timer_get_time() - s_offset_estimate_us - (int64_t)s_latency_offset_ms * 1000;
    int64_t delta = beacon_now_est - s_beat_timestamp_us;
    if (delta < 0) {
        delta = 0; // before the anchor (e.g. we just re-anchored) -- clamp to beat 0
    }
    return delta / period_us;
}

sd_segment_type_t ble_sync_current_pattern(int64_t *progress_beats, int64_t *segment_len_beats)
{
    int64_t beat = ble_sync_beat_index();

    int cur = -1;
    for (int i = 0; i < SD_SCHEDULE_LOOKAHEAD; i++) {
        if ((int64_t)s_segments[i].start_beat <= beat) {
            cur = i;
        }
    }
    if (cur < 0) {
        if (progress_beats) *progress_beats = 0;
        if (segment_len_beats) *segment_len_beats = 0;
        return SD_SEG_STEADY;
    }

    if (progress_beats) {
        *progress_beats = beat - s_segments[cur].start_beat;
    }
    if (segment_len_beats) {
        *segment_len_beats = (cur + 1 < SD_SCHEDULE_LOOKAHEAD)
            ? (int64_t)s_segments[cur + 1].start_beat - (int64_t)s_segments[cur].start_beat
            : 0; // unknown -- this was the last broadcast segment
    }
    return s_segments[cur].type;
}

int32_t ble_sync_latency_offset_ms(void)
{
    return s_latency_offset_ms;
}

void ble_sync_set_latency_offset_ms(int32_t ms)
{
    if (ms < LATENCY_MIN_MS) {
        ms = LATENCY_MIN_MS;
    } else if (ms > LATENCY_MAX_MS) {
        ms = LATENCY_MAX_MS;
    }
    s_latency_offset_ms = ms;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_LATENCY_MS, ms);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_latency_offset(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_LATENCY_MS, &v); // leaves v untouched (0) if the key doesn't exist yet
        nvs_close(h);
    }
    s_latency_offset_ms = v;
}

static void handle_ext_disc(const struct ble_gap_ext_disc_desc *desc)
{
    const struct ble_hs_adv_field *field = NULL;
    if (ble_hs_adv_find_field(BLE_HS_ADV_TYPE_MFG_DATA, desc->data, desc->length_data, &field) != 0
            || field == NULL) {
        return;
    }
    // field->length counts the type byte + value bytes; value[] holds company_id(2) + our packet.
    if (field->length < 1 + 2 + SD_BEACON_PACKET_LEN) {
        return;
    }
    const uint8_t *packet = field->value + 2;
    int packet_len = field->length - 1 - 2;

    sd_beacon_t pkt;
    if (!sd_beacon_unpack(packet, packet_len, &pkt)) {
        return;
    }

    int64_t now = esp_timer_get_time();

    // A changed anchor or channel means the beacon (re)started -- discard
    // offset history rather than blending old and new references.
    if (!s_have_reference || pkt.beat_timestamp_us != s_beat_timestamp_us || pkt.channel_id != s_channel_id) {
        s_sample_count = 0;
        s_next_idx = 0;
        s_have_offset = false;
        s_beat_timestamp_us = pkt.beat_timestamp_us;
        s_channel_id = pkt.channel_id;
        s_have_reference = true;
        ESP_LOGI(TAG, "(re)anchored: channel=%u bpm=%u", pkt.channel_id, pkt.bpm);
    }
    s_bpm = pkt.bpm;
    memcpy(s_segments, pkt.segments, sizeof(s_segments));

    int64_t raw_offset = now - pkt.send_time_us;
    s_samples[s_next_idx].offset_us = raw_offset;
    s_samples[s_next_idx].recv_time_us = now;
    s_next_idx = (s_next_idx + 1) % SAMPLE_CAP;
    if (s_sample_count < SAMPLE_CAP) {
        s_sample_count++;
    }

    // Favor earliest arrivals: the minimum observed offset within the
    // trailing window is the best available clock-offset estimate, since
    // latency only ever adds delay (docs/packet-format.md).
    int64_t best = raw_offset;
    for (int i = 0; i < s_sample_count; i++) {
        if (now - s_samples[i].recv_time_us > WINDOW_US) {
            continue;
        }
        if (s_samples[i].offset_us < best) {
            best = s_samples[i].offset_us;
        }
    }
    s_offset_estimate_us = best;
    s_have_offset = true;
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        handle_ext_disc(&event->ext_disc);
    }
    return 0;
}

static esp_err_t start_scan(void)
{
    struct ble_gap_ext_disc_params uncoded_params = {
        .itvl = 160,  // 100 ms
        .window = 80, // 50 ms
        .passive = 1,
    };

    int rc = ble_gap_ext_disc(BLE_OWN_ADDR_PUBLIC, 0, 0, 0, 0, 0, &uncoded_params, NULL,
                               scan_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_disc failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void on_sync(void)
{
    if (start_scan() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start scan");
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run(); // returns only after nimble_port_stop(), which this app never calls
    nimble_port_freertos_deinit();
}

void ble_sync_init(void)
{
    esp_err_t ret = nvs_flash_init(); // required by the BLE stack for calibration/config data
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_latency_offset();

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}
