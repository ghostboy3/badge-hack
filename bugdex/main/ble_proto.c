#include "ble_proto.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"

static const char *TAG = "ble_proto";

#define ADV_INSTANCE 0
#define BUGDEX_MAGIC0  0xB6
#define BUGDEX_MAGIC1  0xD3
#define BUGDEX_VERSION 1
#define MFG_COMPANY_ID 0xFFFF // unassigned/prototype placeholder, not a registered company ID

#define HEADER_LEN 8 // magic(2) + version(1) + type(1) + sender_id(4)
#define MAX_PACKET_LEN 18 // CATCH_ACK is the longest: header(8) + catcher_id(4) + nonce(4) + flash_delay_ms(2)

#define RADAR_STALE_US (3 * 1000 * 1000) // beacon not heard in 3s -> radar drops to 0

static uint32_t s_own_id;
static uint8_t s_own_species;
static QueueHandle_t s_event_queue;

static volatile int8_t s_radar_rssi = -127;
static volatile int64_t s_radar_last_seen_us = 0;
static volatile uint32_t s_radar_host_id = 0;
static volatile uint8_t s_radar_host_species = 0;

uint32_t bugdex_own_id(void)
{
    return s_own_id;
}

uint8_t bugdex_own_species(void)
{
    return s_own_species;
}

int8_t radar_rssi(void)
{
    if (esp_timer_get_time() - s_radar_last_seen_us > RADAR_STALE_US) {
        return -127;
    }
    return s_radar_rssi;
}

/* Tightened for a small judging area: anything weaker than -70 dBm (roughly
 * "somewhere else in the room") reads as no signal at all, and the whole
 * meter is compressed into the close-range band above that instead of
 * ramping gradually from long range. */
int radar_level(void)
{
    int8_t rssi = radar_rssi();
    if (rssi <= -70) {
        return 0;
    }
    if (rssi <= -62) {
        return 1;
    }
    if (rssi <= -54) {
        return 2;
    }
    if (rssi <= -46) {
        return 3;
    }
    if (rssi <= -40) {
        return 4;
    }
    return 5;
}

uint32_t radar_host_id(void)
{
    return (radar_level() > 0) ? s_radar_host_id : 0;
}

uint8_t radar_host_species(void)
{
    return (radar_level() > 0) ? s_radar_host_species : 0;
}

bool ble_proto_recv(bugdex_event_t *out, TickType_t wait_ticks)
{
    return xQueueReceive(s_event_queue, out, wait_ticks) == pdTRUE;
}

/* Any deterministic mix works here -- GAME.md only requires that species
 * depend on badge_id, not sequential MACs mapping to sequential species. */
static uint32_t hash_badge_id(uint32_t id)
{
    uint32_t x = id;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void write_u32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t read_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void write_header(uint8_t *out, bugdex_pkt_type_t type)
{
    out[0] = BUGDEX_MAGIC0;
    out[1] = BUGDEX_MAGIC1;
    out[2] = BUGDEX_VERSION;
    out[3] = type;
    write_u32(&out[4], s_own_id);
}

static int adv_event_cb(struct ble_gap_event *event, void *arg)
{
    // BEACON/CATCH_* all run with duration 0 (no expiration); nothing to react to here.
    return 0;
}

/* Wraps `packet` as a Manufacturer Specific Data AD element and pushes it
 * onto the already-running extended-adv instance -- it keeps broadcasting
 * whatever was last set here at the configured 100-300ms interval until
 * this is called again. */
static esp_err_t send_raw_packet(const uint8_t *packet, uint8_t packet_len)
{
    uint8_t ad[4 + MAX_PACKET_LEN];
    ad[0] = 1 + 2 + packet_len; // AD type byte + company id(2) + payload
    ad[1] = BLE_HS_ADV_TYPE_MFG_DATA;
    ad[2] = (uint8_t)(MFG_COMPANY_ID & 0xFF);
    ad[3] = (uint8_t)((MFG_COMPANY_ID >> 8) & 0xFF);
    memcpy(&ad[4], packet, packet_len);
    uint8_t ad_len = 4 + packet_len;

    struct os_mbuf *data = os_msys_get_pkthdr(ad_len, 0);
    if (data == NULL) {
        ESP_LOGE(TAG, "os_msys_get_pkthdr failed");
        return ESP_ERR_NO_MEM;
    }
    if (os_mbuf_append(data, ad, ad_len) != 0) {
        ESP_LOGE(TAG, "os_mbuf_append failed");
        os_mbuf_free_chain(data);
        return ESP_FAIL;
    }

    int rc = ble_gap_ext_adv_set_data(ADV_INSTANCE, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_set_data failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ble_proto_send_beacon(void)
{
    uint8_t packet[HEADER_LEN + 2];
    write_header(packet, BUGDEX_PKT_BEACON);
    packet[8] = s_own_species;
    packet[9] = 0; // state: 0 = idle
    send_raw_packet(packet, sizeof(packet));
}

void ble_proto_send_catch_req(uint32_t target_id, uint32_t nonce)
{
    uint8_t packet[HEADER_LEN + 8];
    write_header(packet, BUGDEX_PKT_CATCH_REQ);
    write_u32(&packet[8], target_id);
    write_u32(&packet[12], nonce);
    send_raw_packet(packet, sizeof(packet));
}

void ble_proto_send_catch_ack(uint32_t catcher_id, uint32_t nonce, uint16_t flash_delay_ms)
{
    uint8_t packet[HEADER_LEN + 10];
    write_header(packet, BUGDEX_PKT_CATCH_ACK);
    write_u32(&packet[8], catcher_id);
    write_u32(&packet[12], nonce);
    packet[16] = (uint8_t)(flash_delay_ms & 0xFF);
    packet[17] = (uint8_t)((flash_delay_ms >> 8) & 0xFF);
    send_raw_packet(packet, sizeof(packet));
}

void ble_proto_send_catch_result(uint32_t nonce, bool ok, uint16_t press_delta_ms)
{
    uint8_t packet[HEADER_LEN + 7];
    write_header(packet, BUGDEX_PKT_CATCH_RESULT);
    write_u32(&packet[8], nonce);
    packet[12] = ok ? 1 : 0;
    packet[13] = (uint8_t)(press_delta_ms & 0xFF);
    packet[14] = (uint8_t)((press_delta_ms >> 8) & 0xFF);
    send_raw_packet(packet, sizeof(packet));
}

static esp_err_t start_beacon_adv(void)
{
    struct ble_gap_ext_adv_params params;
    memset(&params, 0, sizeof(params));
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.tx_power = 127;
    params.itvl_min = 160; // 100 ms, per GAME.md section 5 ("about 100-300 ms")
    params.itvl_max = 480; // 300 ms
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &params, NULL, adv_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_configure failed: %d", rc);
        return ESP_FAIL;
    }

    ble_proto_send_beacon(); // sets the initial adv data before starting

    rc = ble_gap_ext_adv_start(ADV_INSTANCE, 0, 0); // no duration/max-events limit
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void handle_ext_disc(const struct ble_gap_ext_disc_desc *desc)
{
    const struct ble_hs_adv_field *field = NULL;
    if (ble_hs_adv_find_field(BLE_HS_ADV_TYPE_MFG_DATA, desc->data, desc->length_data, &field) != 0
            || field == NULL) {
        return;
    }
    // field->length counts the type byte + value bytes; value[] holds company_id(2) + our packet.
    if (field->length < 1 + 2 + HEADER_LEN) {
        return;
    }
    const uint8_t *packet = field->value + 2;
    int packet_len = field->length - 1 - 2;

    if (packet[0] != BUGDEX_MAGIC0 || packet[1] != BUGDEX_MAGIC1 || packet[2] != BUGDEX_VERSION) {
        return;
    }
    bugdex_pkt_type_t type = (bugdex_pkt_type_t)packet[3];
    uint32_t sender_id = read_u32(&packet[4]);
    if (sender_id == s_own_id) {
        return; // ignore our own packets if we ever hear them echoed
    }

    if (type == BUGDEX_PKT_BEACON) {
        if (packet_len < HEADER_LEN + 2) {
            return;
        }
        int64_t now = esp_timer_get_time();
        if (now - s_radar_last_seen_us > RADAR_STALE_US) {
            s_radar_rssi = desc->rssi; // stale -- snap straight to the new reading
        } else {
            s_radar_rssi = (int8_t)((s_radar_rssi * 7 + desc->rssi * 3) / 10); // moving average
        }
        s_radar_last_seen_us = now;
        s_radar_host_id = sender_id;
        s_radar_host_species = packet[8];
        return;
    }

    bugdex_event_t evt = { .type = type, .sender_id = sender_id, .rssi = desc->rssi };
    switch (type) {
    case BUGDEX_PKT_CATCH_REQ:
        if (packet_len < HEADER_LEN + 8) return;
        evt.catch_req.target_id = read_u32(&packet[8]);
        evt.catch_req.nonce = read_u32(&packet[12]);
        break;
    case BUGDEX_PKT_CATCH_ACK:
        if (packet_len < HEADER_LEN + 10) return;
        evt.catch_ack.catcher_id = read_u32(&packet[8]);
        evt.catch_ack.nonce = read_u32(&packet[12]);
        evt.catch_ack.flash_delay_ms = (uint16_t)packet[16] | ((uint16_t)packet[17] << 8);
        break;
    case BUGDEX_PKT_CATCH_RESULT:
        if (packet_len < HEADER_LEN + 7) return;
        evt.catch_result.nonce = read_u32(&packet[8]);
        evt.catch_result.ok = packet[12];
        evt.catch_result.press_delta_ms = (uint16_t)packet[13] | ((uint16_t)packet[14] << 8);
        break;
    default:
        return; // SELFIE_* land in a later step
    }

    xQueueSend(s_event_queue, &evt, 0); // drop if the queue is full; packets repeat anyway
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
        .passive = 1, // passive scan runs whenever the game screen is open (GAME.md section 5)
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
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    s_own_id = (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | (uint32_t)mac[5];
    s_own_species = (uint8_t)(hash_badge_id(s_own_id) % BUGDEX_SPECIES_COUNT);

    ESP_LOGI(TAG, "own id=0x%08lx species=%u", (unsigned long)s_own_id, s_own_species);

    if (start_beacon_adv() != ESP_OK || start_scan() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start beacon/scan");
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

void ble_proto_init(void)
{
    esp_err_t ret = nvs_flash_init(); // required by the BLE stack for calibration/config data
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_event_queue = xQueueCreate(8, sizeof(bugdex_event_t));

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}
