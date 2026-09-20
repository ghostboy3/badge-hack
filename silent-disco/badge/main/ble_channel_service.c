#include "ble_channel_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_channel_svc";

// docs/follow-badge-protocol.md -- keep byte-for-byte in sync with
// web/public/client.js's SERVICE_UUID/CHANNEL_CHAR_UUID. NimBLE's
// BLE_UUID128_INIT takes the bytes in reverse of the dashed string form.
// Service:                9ac33a43-5fe6-40a9-8a5f-921a1a8933e8
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0xe8, 0x33, 0x89, 0x1a, 0x1a, 0x92, 0x5f, 0x8a,
    0xa9, 0x40, 0xe6, 0x5f, 0x43, 0x3a, 0xc3, 0x9a);
// Channel characteristic:  4e6c2458-d8ee-4401-8b35-41819926f033
static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0x33, 0xf0, 0x26, 0x99, 0x81, 0x41, 0x35, 0x8b,
    0x01, 0x44, 0xee, 0xd8, 0x58, 0x24, 0x6c, 0x4e);

static uint8_t s_channel_value;
static uint16_t s_channel_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;
static uint8_t s_addr_type;
static char s_device_name[20]; // "SilentDisco-XXXX"

static void start_advertising(void);

static int channel_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, &s_channel_value, sizeof(s_channel_value));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_uuid.u,
                .access_cb = channel_access_cb,
                .val_handle = &s_channel_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static void notify_channel(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return; // no phone connected/subscribed -- nothing to push
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_channel_value, sizeof(s_channel_value));
    if (om == NULL) {
        ESP_LOGE(TAG, "ble_hs_mbuf_from_flat failed");
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_channel_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "notify failed: %d", rc);
    }
}

void ble_channel_service_set_channel(uint8_t channel)
{
    s_channel_value = channel;
    notify_channel();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            start_advertising(); // connection attempt failed -- stay followable
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_channel_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "subscribe: %d", s_subscribed);
        }
        return 0;

    default:
        return 0;
    }
}

/* Primary adv packet: flags + the 128-bit service UUID (Chrome's
 * requestDevice({filters:[{services:[...]}]}) only matches UUIDs actually
 * advertised, not ones discovered after connecting). The device name goes
 * in the scan response instead -- flags+UUID already use 21 of the legacy
 * advertisement's 31-byte budget, no room left for a name too. See
 * docs/follow-badge-protocol.md. */
static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)s_device_name;
    rsp_fields.name_len = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    snprintf(s_device_name, sizeof(s_device_name), "SilentDisco-%02X%02X", mac[4], mac[5]);
    ble_svc_gap_device_name_set(s_device_name);
    ESP_LOGI(TAG, "device name: %s", s_device_name);

    start_advertising();
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

void ble_channel_service_init(void)
{
    esp_err_t ret = nvs_flash_init(); // required by the BLE stack for calibration/config data
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);
}
