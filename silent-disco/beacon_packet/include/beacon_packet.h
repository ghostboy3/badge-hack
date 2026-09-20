#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Wire format shared by beacon/ and badge/ via the beacon_packet component
 * -- see docs/packet-format.md. Keep this the single source of truth; do
 * not copy-paste it elsewhere. */

#define SD_MAGIC0  0x5D
#define SD_MAGIC1  0x15
#define SD_VERSION 2 // v2 (step 4b): adds the pattern-segment lookahead

typedef enum {
    SD_PKT_BEACON = 0x01,
} sd_pkt_type_t;

/* CLAUDE.md rule 1: "a short lookahead of pattern data (build-up, drop,
 * breakdown)". Badges/phones only ever consume this list; the algorithm
 * that generates it (beacon/main/pattern.c) is the beacon's business, and
 * can become a real DJ- or track-analysis-driven schedule later without
 * touching badge firmware. */
typedef enum {
    SD_SEG_STEADY    = 0,
    SD_SEG_BUILDUP   = 1,
    SD_SEG_DROP      = 2,
    SD_SEG_BREAKDOWN = 3,
} sd_segment_type_t;

#define SD_SCHEDULE_LOOKAHEAD 3 // current segment + this many upcoming

typedef struct {
    sd_segment_type_t type;
    uint32_t start_beat; // absolute beat index (0 = the beat at beat_timestamp_us) when this segment begins
} sd_segment_t;

// magic(2)+version(1)+type(1)+channel(1)+bpm(1)+beat_timestamp_us(8)+send_time_us(8)+segments(3*5)
#define SD_BEACON_PACKET_LEN (22 + SD_SCHEDULE_LOOKAHEAD * 5)

typedef struct {
    uint8_t channel_id;         // fixed 0 in step 1
    uint8_t bpm;                // fixed 120 in step 1
    int64_t beat_timestamp_us;  // beacon-clock time of beat 0 (fixed anchor, set once at boot)
    int64_t send_time_us;       // beacon-clock time this packet was assembled (refreshed every broadcast)
    sd_segment_t segments[SD_SCHEDULE_LOOKAHEAD]; // segments[0] is the one covering "now" at send_time_us
} sd_beacon_t;

void sd_beacon_pack(const sd_beacon_t *in, uint8_t out[SD_BEACON_PACKET_LEN]);

/* Validates magic/version/type and unpacks on success. `len` is the number
 * of bytes available at `data` (may be longer than SD_BEACON_PACKET_LEN). */
bool sd_beacon_unpack(const uint8_t *data, int len, sd_beacon_t *out);
