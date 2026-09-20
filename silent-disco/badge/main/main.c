/*
 * Silent Disco badge -- local channel color, always-on breathing fade.
 *
 * The badge no longer needs to hear anything to look good: no BLE beacon
 * scanning, no beat sync. LEDs continuously fade bright<->dim in the
 * selected channel's color on the badge's own free-running clock. Left/
 * Right cycles the channel (still purely local, per CLAUDE.md's
 * architecture -- "phones and badges never connect to each other").
 *
 * This replaces the earlier beat-synced/pattern-synced LED rendering and
 * the "Sync check" latency-nudge screen (both depended on hearing a
 * beacon over BLE) -- removed along with ble_sync.c. beacon/ and
 * docs/packet-format.md still describe a working BLE beat broadcast; the
 * badge just doesn't consume it anymore.
 *
 * BLE is back, though, in a different role: ble_channel_service.c runs a
 * connectable GATT peripheral exposing the current channel, volume and
 * mute state so a phone can optionally "follow" it
 * (docs/follow-badge-protocol.md). The badge has no speaker itself --
 * Up/Down/A only ever mean anything to a phone that's actively following.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "leds.h"
#include "buttons.h"
#include "display.h"
#include "ble_channel_service.h"

static const char *TAG = "badge";

#define TICK_MS 15

#define BREATHE_PERIOD_MS 3000 // one full bright -> dim -> bright cycle
#define BREATHE_MIN_PCT    15  // dimmest point -- still visibly lit, never fully off
#define BREATHE_MAX_PCT   100

typedef struct {
    const char *name;
    uint8_t r, g, b;
} channel_def_t;

#define CHANNEL_COUNT 3

#define VOLUME_STEP_PCT    10
#define DEFAULT_VOLUME_PCT 70 // matches ble_channel_service.c's initial state

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
static uint8_t s_volume_pct = DEFAULT_VOLUME_PCT; // Up/Down; only ui_task touches these, no cross-task read
static bool s_muted;

// Triangle wave 0..1000..0 (parts-per-thousand) over BREATHE_PERIOD_MS,
// mapped onto [BREATHE_MIN_PCT, BREATHE_MAX_PCT]. Pure integer math.
static int breathe_brightness_pct(uint32_t elapsed_ms)
{
    uint32_t phase_ms = elapsed_ms % BREATHE_PERIOD_MS;
    uint32_t half = BREATHE_PERIOD_MS / 2;
    uint32_t triangle_ppt = (phase_ms < half)
        ? (phase_ms * 1000) / half
        : ((BREATHE_PERIOD_MS - phase_ms) * 1000) / half;
    return BREATHE_MIN_PCT + (int)((BREATHE_MAX_PCT - BREATHE_MIN_PCT) * triangle_ppt / 1000);
}

static void led_render_task(void *arg)
{
    uint32_t elapsed_ms = 0;

    while (1) {
        int brightness_pct = breathe_brightness_pct(elapsed_ms);
        const channel_def_t *ch = &CHANNELS[s_local_channel];
        int r = (ch->r * brightness_pct) / 100;
        int g = (ch->g * brightness_pct) / 100;
        int b = (ch->b * brightness_pct) / 100;

        for (int i = 0; i < LED_COUNT; i++) {
            leds_set(i, r, g, b);
        }
        leds_refresh();

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        elapsed_ms += TICK_MS;
    }
}

static void render_display(void)
{
    if (!s_display_ok) {
        return;
    }
    const channel_def_t *ch = &CHANNELS[s_local_channel];
    char vol_text[16];
    if (s_muted) {
        snprintf(vol_text, sizeof(vol_text), "Muted");
    } else {
        snprintf(vol_text, sizeof(vol_text), "Vol: %u%%", s_volume_pct);
    }
    char text[96];
    snprintf(text, sizeof(text), "Silent Disco\n%s (CH%u)  %s\nLeft/Right:ch Up/Dn:vol A:mute",
             ch->name, s_local_channel, vol_text);
    display_show_text(text);
}

static void ui_task(void *arg)
{
    buttons_init();
    render_display();

    while (1) {
        buttons_poll();

        if (button_was_pressed(BUTTON_LEFT)) {
            s_local_channel = (s_local_channel + CHANNEL_COUNT - 1) % CHANNEL_COUNT;
            render_display();
            ble_channel_service_set_channel(s_local_channel);
        } else if (button_was_pressed(BUTTON_RIGHT)) {
            s_local_channel = (s_local_channel + 1) % CHANNEL_COUNT;
            render_display();
            ble_channel_service_set_channel(s_local_channel);
        } else if (button_was_pressed(BUTTON_UP)) {
            s_muted = false; // volume buttons unmute, same as a phone's hardware volume rocker
            s_volume_pct = (s_volume_pct + VOLUME_STEP_PCT > 100) ? 100 : s_volume_pct + VOLUME_STEP_PCT;
            render_display();
            ble_channel_service_set_volume(s_volume_pct, s_muted);
        } else if (button_was_pressed(BUTTON_DOWN)) {
            s_muted = false;
            s_volume_pct = (s_volume_pct < VOLUME_STEP_PCT) ? 0 : s_volume_pct - VOLUME_STEP_PCT;
            render_display();
            ble_channel_service_set_volume(s_volume_pct, s_muted);
        } else if (button_was_pressed(BUTTON_A)) {
            s_muted = !s_muted;
            render_display();
            ble_channel_service_set_volume(s_volume_pct, s_muted);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Silent Disco badge: local color, breathing fade, follow-my-badge BLE");

    leds_init();
    ble_channel_service_init();

    s_display_ok = (display_init() != NULL);
    if (!s_display_ok) {
        ESP_LOGE(TAG, "display init failed -- continuing without a screen");
    }

    xTaskCreate(led_render_task, "led_render_task", 4096, NULL, 5, NULL);
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL);
}
