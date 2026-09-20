#pragma once

/* Starts NimBLE (init once at boot, never deinit -- custom-firmware-hal.md
 * section 8) and a non-connectable extended-adv instance broadcasting the
 * SD_PKT_BEACON payload (docs/packet-format.md). A background task
 * refreshes send_time_us and re-sets the adv data every ~150 ms so the
 * badge side can keep estimating clock offset; the radio itself repeats
 * whatever was last set at the configured 100-300 ms interval in between. */
void ble_beacon_init(void);
