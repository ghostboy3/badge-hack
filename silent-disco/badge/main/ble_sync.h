#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "beacon_packet.h" // sd_segment_type_t

/* Starts NimBLE (init once at boot, never deinit -- custom-firmware-hal.md
 * section 8) and passive extended scanning for the beacon's SD_PKT_BEACON
 * payload (docs/packet-format.md). Estimates clock offset to the beacon
 * from several packets, favoring earliest arrivals, and free-runs between
 * receptions -- lost packets don't affect the exposed phase. */
void ble_sync_init(void);

bool ble_sync_synced(void); // true once at least one offset estimate exists

uint8_t ble_sync_channel(void);
uint8_t ble_sync_bpm(void);

/* Microseconds since the most recent beat onset, already shifted by the
 * latency offset below; 0 exactly on a beat, wraps at the beat period.
 * Only meaningful once ble_sync_synced(). */
int64_t ble_sync_phase_us(void);

/* Per-badge latency offset (CLAUDE.md rule 4, "Sync check" screen): delays
 * the beat trigger by this many ms so it lands when the wearer's own
 * Bluetooth headphones actually play the beat, not when it "truly" occurs.
 * Persisted to NVS; loaded once at ble_sync_init(). Clamped to
 * [0, 500] ms -- Bluetooth audio is only ever late, never early, so there's
 * no reason to support a negative shift. */
int32_t ble_sync_latency_offset_ms(void);
void ble_sync_set_latency_offset_ms(int32_t ms);

/* Absolute beat index (0 = the beat at the beacon's anchor), already
 * shifted by the latency offset, consistent with ble_sync_phase_us(). */
int64_t ble_sync_beat_index(void);

/* Looks up which of the most recently broadcast lookahead segments
 * (docs/packet-format.md) covers the current beat. Returns SD_SEG_STEADY
 * if no segment is known yet (not synced). If non-NULL, *progress_beats
 * receives how many beats into that segment we are, and *segment_len_beats
 * receives the segment's length in beats -- 0 if unknown, which happens
 * when the current segment is the last of the broadcast lookahead and no
 * "next" boundary has been received yet. */
sd_segment_type_t ble_sync_current_pattern(int64_t *progress_beats, int64_t *segment_len_beats);
