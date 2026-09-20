/*
 * Silent Disco beacon -- step 1 per silent-disco/CLAUDE.md's build order.
 *
 * Fixed single channel, fixed 120 BPM. Broadcasts the SD_PKT_BEACON payload
 * (docs/packet-format.md) over BLE extended advertising; badge/ scans for
 * it, estimates clock offset, and pulses LEDs in time.
 */
#include "esp_log.h"

#include "ble_beacon.h"

static const char *TAG = "beacon";

void app_main(void)
{
    ESP_LOGI(TAG, "Silent Disco beacon: step 1 (fixed channel, fixed 120 BPM)");
    ble_beacon_init();
}
