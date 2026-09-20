#pragma once

#include <stdbool.h>
#include <stdint.h>

/* "Follow my badge" (docs/follow-badge-protocol.md): starts NimBLE as a
 * connectable GATT peripheral exposing the badge's current channel, volume
 * and mute state as one 3-byte read+notify characteristic
 * [channel, volume_pct, muted]. Android Chrome's Web Bluetooth can
 * connect, subscribe, and mirror it: switch the phone's channel, scale its
 * playback gain, mute it. Separate, unrelated protocol from the old
 * (currently unused) beacon beat-broadcast -- see docs/packet-format.md. */
void ble_channel_service_init(void);

/* Call whenever the locally-selected channel changes (Left/Right). Updates
 * the exposed value and notifies a subscribed phone, if any. */
void ble_channel_service_set_channel(uint8_t channel);

/* Call whenever volume (Up/Down) or mute (A) changes. volume_pct is
 * clamped to [0, 100]. */
void ble_channel_service_set_volume(uint8_t volume_pct, bool muted);
