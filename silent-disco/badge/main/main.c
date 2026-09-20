/*
 * Silent Disco badge -- steps 1, 3 & 4 per silent-disco/CLAUDE.md's build
 * order (fixed-BPM LED sync, the per-badge latency-nudge "Sync check"
 * screen, local three-channel color selection, and the lookahead pattern
 * schedule).
 *
 * Channel selection is purely local per CLAUDE.md's architecture ("Phones
 * and badges never connect to each other") -- the beacon still broadcasts
 * one shared beat timeline (ble_sync.c), and Left/Right here only changes
 * which color this badge renders on that same timeline. Giving each
 * channel its own independent tempo/pattern is future work, not this step.
 *
 * The beacon also broadcasts a short lookahead of pattern segments
 * (steady/build-up/drop/breakdown, docs/packet-format.md); led_render_task
 * looks up the current one via ble_sync_current_pattern() and varies
 * brightness/pulse-width/sparsity accordingly, same shared timeline as the
 * plain beat flash.
 *
 * ble_sync.c scans for the beacon (beacon/), estimates clock offset, and
 * owns the persisted per-badge latency offset. led_render_task just
 * renders the beat; ui_task owns buttons + the display.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "leds.h"
#include "buttons.h"
#include "display.h"
#include "ble_sync.h"

static const char *TAG = "badge";

#define TICK_MS         15
#define PULSE_WIDTH_US  (60 * 1000) // short flash on the beat

#define SEARCH_BLINK_PERIOD_MS 1000
#define SEARCH_BLINK_ON_MS     100

#define LATENCY_STEP_MS 10

typedef struct {
    const char *name;
    uint8_t r, g, b;
} channel_def_t;

#define CHANNEL_COUNT 3

// Pink/orange/purple, per CLAUDE.md's channel color table -- no green/blue
// (reserved by the Mood app). Kept modest per custom-firmware-hal.md's
// brownout warning for 6 LEDs at once.
static const channel_def_t CHANNELS[CHANNEL_COUNT] = {
    { "Pink",   40,  4, 20 },
    { "Orange", 40, 16,  0 },
    { "Purple", 24,  0, 40 },
};

static bool s_display_ok;
static volatile uint8_t s_local_channel; // written by ui_task (Left/Right), read by led_render_task

typedef enum {
    UI_MODE_NORMAL,
    UI_MODE_SYNC_CHECK,
} ui_mode_t;

static const char *segment_name(sd_segment_type_t seg)
{
    switch (seg) {
    case SD_SEG_BUILDUP:   return "Build-up";
    case SD_SEG_DROP:      return "Drop";
    case SD_SEG_BREAKDOWN: return "Breakdown";
    case SD_SEG_STEADY:
    default:                return "Steady";
    }
}

/* Segment -> rendering knobs. Pure integer math (no float unit needed):
 * brightness_pct scales the channel color, pulse_width_us controls flash
 * duration, flash_this_beat can skip beats for a sparser feel. Numbers are
 * chosen for visual effect, not derived from anything -- tune freely. */
static void pattern_render_params(sd_segment_type_t seg, int64_t progress_beats, int64_t segment_len_beats,
                                   int64_t beat_index, int *brightness_pct, int64_t *pulse_width_us,
                                   bool *flash_this_beat)
{
    *brightness_pct = 100;
    *pulse_width_us = PULSE_WIDTH_US;
    *flash_this_beat = true;

    switch (seg) {
    case SD_SEG_BUILDUP: {
        int pct = (segment_len_beats > 0) ? (int)((progress_beats * 100) / segment_len_beats) : 0;
        if (pct > 100) pct = 100;
        *brightness_pct = 50 + pct / 2;              // ramps 50% -> 100% across the build-up
        *pulse_width_us = PULSE_WIDTH_US + pct * 600; // widens up to +60ms right before the drop
        break;
    }
    case SD_SEG_DROP:
        *brightness_pct = 160; // brighter hit; still well under the 255 cap for these dim base colors
        *pulse_width_us = PULSE_WIDTH_US * 2;
        break;
    case SD_SEG_BREAKDOWN:
        *brightness_pct = 35;                       // dim, calmer
        *flash_this_beat = (beat_index % 2) == 0;    // sparser: every other beat
        break;
    case SD_SEG_STEADY:
    default:
        break;
    }
}

static void led_render_task(void *arg)
{
    uint32_t elapsed_ms = 0;

    while (1) {
        leds_clear();

        if (ble_sync_synced()) {
            int64_t progress_beats, segment_len_beats;
            sd_segment_type_t seg = ble_sync_current_pattern(&progress_beats, &segment_len_beats);
            int64_t beat_index = ble_sync_beat_index();

            int brightness_pct;
            int64_t pulse_width_us;
            bool flash_this_beat;
            pattern_render_params(seg, progress_beats, segment_len_beats, beat_index,
                                   &brightness_pct, &pulse_width_us, &flash_this_beat);

            if (flash_this_beat && ble_sync_phase_us() < pulse_width_us) {
                const channel_def_t *ch = &CHANNELS[s_local_channel];
                int r = (ch->r * brightness_pct) / 100;
                int g = (ch->g * brightness_pct) / 100;
                int b = (ch->b * brightness_pct) / 100;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                for (int i = 0; i < LED_COUNT; i++) {
                    leds_set(i, r, g, b);
                }
            }
        } else if ((elapsed_ms % SEARCH_BLINK_PERIOD_MS) < SEARCH_BLINK_ON_MS) {
            leds_set(0, 12, 12, 12); // dim white, single LED: "looking for a beacon"
        }

        leds_refresh();

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        elapsed_ms += TICK_MS;
    }
}

static void render_display(ui_mode_t mode)
{
    if (!s_display_ok) {
        return;
    }

    char text[96];
    if (mode == UI_MODE_SYNC_CHECK) {
        snprintf(text, sizeof(text), "Sync check\nOffset: %ld ms\nUp/Down to adjust\nHome to exit",
                 (long)ble_sync_latency_offset_ms());
    } else {
        const channel_def_t *ch = &CHANNELS[s_local_channel];
        const char *status = ble_sync_synced() ? segment_name(ble_sync_current_pattern(NULL, NULL)) : "Searching...";
        snprintf(text, sizeof(text), "Silent Disco\n%s (CH%u)  %u BPM\n%s\nHome:sync  </>:channel",
                 ch->name, s_local_channel, ble_sync_bpm(), status);
    }
    display_show_text(text);
}

/* Buttons + the display. Home toggles the "Sync check" screen (CLAUDE.md
 * rule 4): while in it, Up/Down nudge the persisted latency offset in
 * LATENCY_STEP_MS steps. Otherwise Left/Right cycle the locally-rendered
 * channel/color. The badge keeps flashing on the beat throughout --
 * led_render_task reads ble_sync_phase_us() and s_local_channel directly,
 * this task only owns the screen and button handling. */
static void ui_task(void *arg)
{
    buttons_init();

    ui_mode_t mode = UI_MODE_NORMAL;
    render_display(mode);

    int refresh_counter = 0;
    while (1) {
        buttons_poll();
        bool changed = false;

        if (button_was_pressed(BUTTON_HOME)) {
            mode = (mode == UI_MODE_NORMAL) ? UI_MODE_SYNC_CHECK : UI_MODE_NORMAL;
            changed = true;
        }

        if (mode == UI_MODE_SYNC_CHECK) {
            if (button_was_pressed(BUTTON_UP) || button_was_pressed(BUTTON_DOWN)) {
                int32_t step = button_was_pressed(BUTTON_UP) ? LATENCY_STEP_MS : -LATENCY_STEP_MS;
                ble_sync_set_latency_offset_ms(ble_sync_latency_offset_ms() + step);
                changed = true;
            }
        } else {
            if (button_was_pressed(BUTTON_LEFT)) {
                s_local_channel = (s_local_channel + CHANNEL_COUNT - 1) % CHANNEL_COUNT;
                changed = true;
            } else if (button_was_pressed(BUTTON_RIGHT)) {
                s_local_channel = (s_local_channel + 1) % CHANNEL_COUNT;
                changed = true;
            }
        }

        if (changed) {
            render_display(mode);
            refresh_counter = 0;
        } else if (mode == UI_MODE_NORMAL && ++refresh_counter >= 20) { // ~200 ms at this 10 ms tick
            refresh_counter = 0;
            render_display(mode); // keeps the Synced/Searching status fresh
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Silent Disco badge: steps 1+3+4 (fixed BPM, sync check, 3 channels, pattern schedule)");

    leds_init();
    ble_sync_init();

    s_display_ok = (display_init() != NULL);
    if (!s_display_ok) {
        ESP_LOGE(TAG, "display init failed -- continuing without a screen");
    }

    xTaskCreate(led_render_task, "led_render_task", 4096, NULL, 5, NULL);
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
}
