#include "ble_beacon.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#include "pattern.h"

static const char *TAG = "ble_beacon";

#define ADV_INSTANCE      0
#define MFG_COMPANY_ID    0xFFFF // unassigned/prototype placeholder, same as bugdex
#define REFRESH_PERIOD_MS 150    // re-sets adv data with a fresh send_time_us; see docs/packet-format.md

// Step 1: single fixed channel/tempo. Step 2+ makes these configurable.
#define CHANNEL_ID 0
#define BPM        120

static int64_t s_beat_timestamp_us; // fixed anchor, set once when advertising starts

static int adv_event_cb(struct ble_gap_event *event, void *arg)
{
    // Runs with duration 0 (no expiration); nothing to react to here.
    return 0;
}

/* Wraps `packet` as a Manufacturer Specific Data AD element and pushes it
 * onto the running extended-adv instance, same shape as
 * bugdex/main/ble_proto.c's send_raw_packet. */
static esp_err_t send_raw_packet(const uint8_t *packet, uint8_t packet_len)
{
    uint8_t ad[4 + SD_BEACON_PACKET_LEN];
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

static void broadcast_beacon(void)
{
    int64_t now = esp_timer_get_time();
    int64_t beat_now = (now - s_beat_timestamp_us) / (60000000LL / BPM);

    sd_beacon_t beacon = {
        .channel_id = CHANNEL_ID,
        .bpm = BPM,
        .beat_timestamp_us = s_beat_timestamp_us,
        .send_time_us = now,
    };
    pattern_lookahead(beat_now, beacon.segments);

    uint8_t packet[SD_BEACON_PACKET_LEN];
    sd_beacon_pack(&beacon, packet);
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
    params.itvl_min = 160; // 100 ms
    params.itvl_max = 480; // 300 ms
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &params, NULL, adv_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_configure failed: %d", rc);
        return ESP_FAIL;
    }

    s_beat_timestamp_us = esp_timer_get_time(); // this instant becomes beat 0
    broadcast_beacon(); // sets the initial adv data before starting

    rc = ble_gap_ext_adv_start(ADV_INSTANCE, 0, 0); // no duration/max-events limit
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void refresh_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REFRESH_PERIOD_MS));
        broadcast_beacon();
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "channel=%d bpm=%d", CHANNEL_ID, BPM);

    if (start_beacon_adv() != ESP_OK) {
        ESP_LOGE(TAG, "failed to start beacon adv");
        return;
    }
    xTaskCreate(refresh_task, "beacon_refresh", 3072, NULL, 5, NULL);
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

void ble_beacon_init(void)
{
    esp_err_t ret = nvs_flash_init(); // required by the BLE stack for calibration/config data
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}
