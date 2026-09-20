#include "pattern.h"

/* A fixed, deterministic "song structure" standing in for a real DJ- or
 * track-analysis-driven schedule (CLAUDE.md rule 5: pre-analyzing real
 * tracks offline is future work, not this step). Badges never see this
 * cycle -- they only ever consume the segment list this broadcasts -- so
 * swapping it for a real schedule later only touches this file. */
#define CYCLE_BEATS 32

typedef struct {
    uint32_t offset_in_cycle;
    sd_segment_type_t type;
} boundary_t;

static const boundary_t BOUNDARIES[] = {
    { 0,  SD_SEG_STEADY },    // 16 beats (~8s @ 120bpm)
    { 16, SD_SEG_BUILDUP },   // 8 beats (~4s)
    { 24, SD_SEG_DROP },      // 2 beats (~1s)
    { 26, SD_SEG_BREAKDOWN }, // 6 beats (~3s), then the cycle repeats
};
#define BOUNDARY_COUNT (int)(sizeof(BOUNDARIES) / sizeof(BOUNDARIES[0]))

void pattern_lookahead(int64_t beat_now, sd_segment_t out[SD_SCHEDULE_LOOKAHEAD])
{
    int64_t cycle_index = beat_now / CYCLE_BEATS;
    uint32_t pos = (uint32_t)(beat_now - cycle_index * CYCLE_BEATS);

    int cur = 0;
    for (int i = 0; i < BOUNDARY_COUNT; i++) {
        if (BOUNDARIES[i].offset_in_cycle <= pos) {
            cur = i;
        }
    }

    for (int j = 0; j < SD_SCHEDULE_LOOKAHEAD; j++) {
        int idx = (cur + j) % BOUNDARY_COUNT;
        int64_t extra_cycles = (cur + j) / BOUNDARY_COUNT;
        out[j].type = BOUNDARIES[idx].type;
        out[j].start_beat = (uint32_t)((cycle_index + extra_cycles) * CYCLE_BEATS + BOUNDARIES[idx].offset_in_cycle);
    }
}
