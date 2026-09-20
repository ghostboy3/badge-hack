/*
 * Bugdex -- step 4 (dex, met-log, 30-minute rule) per GAME.md's build order.
 *
 * - BLE beacon + radar (ble_proto.c, from step 2) is unchanged.
 * - game_state.c owns button polling and the whole IDLE/CATCH state machine,
 *   and now also the dex/met-log storage calls on a successful catch.
 * - storage.c (new) persists the dex and met-log to the "storage" flash
 *   partition -- initialized here, before anything can call into it.
 * - radar_led_task (blue host-pulse + amber radar) only draws while IDLE --
 *   game_state.c takes exclusive control of the LEDs during a catch (white
 *   flash, celebration) and this task steps back rather than fighting it.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display.h"
#include "leds.h"
#include "ble_proto.h"
#include "game_state.h"
#include "storage.h"

static const char *TAG = "bugdex";

#define HOST_PULSE_ON_MS     150
#define HOST_PULSE_PERIOD_MS 1300 // within GAME.md's 1-1.5s range

// Amber pulse period per radar level (0 = no signal -> LEDs off); faster as you get closer.
static const int s_radar_period_ms[6] = { 0, 1000, 700, 450, 250, 120 };

static void radar_led_task(void *arg)
{
    leds_init();

    uint32_t elapsed_ms = 0;
    const int tick_ms = 20;

    while (1) {
        if (game_state_current() == GAME_STATE_IDLE) {
            leds_clear();

            // LEDs 0-1: purple "I'm hosting a bug" pulse, while idle -- but
            // not if a stronger/lower-ID host is nearby (demo clarity: two
            // hosts sitting right next to each other shouldn't both show
            // as "hosting" at once). Both badges evaluate the same
            // comparison, so exactly one of them shows it in that case.
            uint32_t host_id = radar_host_id();
            bool show_host_led = (host_id == 0) || (bugdex_own_id() < host_id);
            if (show_host_led && (elapsed_ms % HOST_PULSE_PERIOD_MS) < HOST_PULSE_ON_MS) {
                leds_set(0, 20, 0, 24);
                leds_set(1, 20, 0, 24);
            }

            // LEDs 2-5: amber radar pulse, rate scales with proximity.
            int level = radar_level();
            int period = s_radar_period_ms[level];
            if (period > 0 && (elapsed_ms % period) < (period / 2)) {
                for (int i = 2; i <= 5; i++) {
                    leds_set(i, 24, 12, 0);
                }
            }

            leds_refresh();
        }
        // else: a catch is in progress -- game_state.c owns the LEDs.

        vTaskDelay(pdMS_TO_TICKS(tick_ms));
        elapsed_ms += tick_ms;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Bugdex step 4: dex, met-log, 30-minute rule");

    storage_init();
    ble_proto_init();

    xTaskCreate(radar_led_task, "radar_led_task", 4096, NULL, 5, NULL);

    if (display_init() == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    display_show_text("Bugdex\nwaiting for BLE sync...");

    game_state_init();
}
