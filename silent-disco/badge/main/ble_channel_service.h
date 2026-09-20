#pragma once

#include <stdint.h>

/* "Follow my badge" (docs/follow-badge-protocol.md): starts NimBLE as a
 * connectable GATT peripheral exposing the badge's current channel (0/1/2)
 * as a read+notify characteristic. Android Chrome's Web Bluetooth can
 * connect, subscribe, and switch the phone's channel to match. Separate,
 * unrelated protocol from the old (currently unused) beacon beat-broadcast
 * -- see docs/packet-format.md. */
void ble_channel_service_init(void);

/* Call whenever the locally-selected channel changes (Left/Right). Updates
 * the exposed value and notifies a subscribed phone, if any. */
void ble_channel_service_set_channel(uint8_t channel);
