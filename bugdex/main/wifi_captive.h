#pragma once

/* SoftAP lifecycle for selfie Stage A (GAME.md section 7). Open network,
 * started only for the duration of the selfie exchange -- BLE stays
 * initialized throughout (never deinit, per custom-firmware-hal.md section
 * 8), so this is also the heap-headroom check for WiFi+BLE coexistence. */
void wifi_captive_start(const char *ssid);
void wifi_captive_stop(void);
