#pragma once

#include <stdint.h>
#include "beacon_packet.h"

/* Fills out[SD_SCHEDULE_LOOKAHEAD] with the segment covering `beat_now`
 * (out[0]) and the next SD_SCHEDULE_LOOKAHEAD-1 segments after it, each
 * with its absolute start_beat -- see docs/packet-format.md. `beat_now`
 * must be >= 0 (true from the moment beat_timestamp_us is set, since it's
 * only ever compared against times at or after that). */
void pattern_lookahead(int64_t beat_now, sd_segment_t out[SD_SCHEDULE_LOOKAHEAD]);
