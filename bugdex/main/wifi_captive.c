#include "wifi_captive.h"

#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

static const char *TAG = "wifi_captive";
static bool s_wifi_ready = false; // esp_wifi_init() etc. done once; NVS is already inited by ble_proto_init()

static void wifi_init_once(void)
{
    if (s_wifi_ready) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    s_wifi_ready = true;
}

void wifi_captive_start(const char *ssid)
{
    wifi_init_once();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = 6,
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN, // open network, per GAME.md section 7
        },
    };
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP '%s' up -- free heap: %u bytes (internal: %u, largest block: %u)",
             ssid, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void wifi_captive_stop(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_LOGI(TAG, "AP stopped -- free heap: %u bytes", (unsigned)esp_get_free_heap_size());
}
