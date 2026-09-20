/*
 * Bugdex -- step 4 (dex, met-log, 30-minute rule) per GAME.md's build order,
 * on top of step 3's sync-catch state machine (section 6). Owns buttons.c's
 * polling itself (see game_state.h) so it gets first-hand button-press
 * events instead of going through a separate task's edge flags.
 *
 * No timing window on the press itself: a catch succeeds as long as both
 * people press A after the cue, whenever they get to it (GAME.md was
 * updated to match). Only the catcher's dex grows on a successful catch;
 * both sides record the encounter in the met-log so the 30-minute cooldown
 * is enforced from either direction (GAME.md: "Enforce on both sides").
 */
#include "game_state.h"

#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "buttons.h"
#include "display.h"
#include "leds.h"
#include "ble_proto.h"
#include "storage.h"
#include "wifi_captive.h"

static const char *TAG = "game_state";

// GAME.md section 6: "-60 dBm for 2s before CATCH_REQ is allowed."
#define CATCH_RSSI_THRESHOLD (-60)
#define CATCH_RSSI_HOLD_US   (2 * 1000 * 1000)

#define CATCH_COOLDOWN_SECONDS (30 * 60) // GAME.md section 3: once every 30 minutes, enforced both sides

// Demo mode: the 30-minute anti-farming cooldown makes repeated demo catches
// annoying to work around. Flip this back to 1 to restore the real rule.
#define ANTI_FARMING_ENABLED 0

#define ACK_TIMEOUT_MS    3000
#define PRESS_WINDOW_MS   8000 // generous -- no reaction-time requirement, just "did you press A"
#define RESULT_TIMEOUT_MS 8000

static uint32_t now_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static volatile game_state_t s_state = GAME_STATE_IDLE;
static int64_t s_strong_since_us = 0;

game_state_t game_state_current(void)
{
    return s_state;
}

static bool wait_for_press_a(int64_t deadline_us)
{
    while (esp_timer_get_time() < deadline_us) {
        buttons_poll();
        if (button_was_pressed(BUTTON_A)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* Shared by both roles: wait for the cue, flash, record the press, exchange
 * CATCH_RESULT, and show the outcome. `peer_id`/`nonce` identify who we're
 * catching/being caught by and which exchange this is. `is_catcher` and
 * `caught_species` control what gets written to storage on success -- only
 * the catcher's dex grows; `caught_species` is meaningless when
 * `is_catcher` is false. */
static void run_cue_and_result(uint32_t peer_id, uint32_t nonce, uint16_t flash_delay_ms,
                                bool is_catcher, uint8_t caught_species)
{
    s_state = GAME_STATE_CATCH;

    display_show_text("Get ready...");
    vTaskDelay(pdMS_TO_TICKS(flash_delay_ms));

    int64_t cue_time_us = esp_timer_get_time(); // "local time when the flash starts"
    leds_clear();
    for (int i = 0; i < LED_COUNT; i++) {
        leds_set(i, 40, 40, 40); // white flash
    }
    leds_refresh();
    display_show_text("PRESS A!");

    bool pressed = wait_for_press_a(cue_time_us + (int64_t)PRESS_WINDOW_MS * 1000);
    int64_t press_time_us = esp_timer_get_time();

    leds_clear();
    leds_refresh();

    // No timing requirement -- pressing A at all (within the window) is enough.
    bool ok = pressed;
    uint16_t delta_ms = pressed ? (uint16_t)((press_time_us - cue_time_us) / 1000) : 0xFFFF;
    ESP_LOGI(TAG, "local press ok=%d delta=%u ms (informational only)", ok, delta_ms);

    ble_proto_send_catch_result(nonce, ok, delta_ms);
    display_show_text(ok ? "Sent!\nWaiting for partner..." : "No press.\nWaiting for partner...");

    int64_t result_deadline = esp_timer_get_time() + (int64_t)RESULT_TIMEOUT_MS * 1000;
    bool peer_ok = false;
    bool peer_seen = false;
    while (esp_timer_get_time() < result_deadline) {
        bugdex_event_t evt;
        if (ble_proto_recv(&evt, pdMS_TO_TICKS(50))
                && evt.type == BUGDEX_PKT_CATCH_RESULT
                && evt.catch_result.nonce == nonce
                && evt.sender_id == peer_id) {
            peer_ok = evt.catch_result.ok;
            peer_seen = true;
            break;
        }
    }

    bool success = ok && peer_seen && peer_ok;
    if (success) {
        uint32_t now = now_seconds();
        storage_record_catch(peer_id, now); // both sides log the encounter, for the 30-min rule
        if (is_catcher) {
            storage_dex_add(caught_species, peer_id, now, 0xFF);
        }

        ESP_LOGI(TAG, "CATCH SUCCESS with peer=0x%08lx (dex_count=%d)",
                 (unsigned long)peer_id, storage_dex_count());
        display_show_text("Caught!");
        for (int rep = 0; rep < 3; rep++) { // dim celebration flash -- AA power warning
            leds_clear();
            for (int i = 0; i < LED_COUNT; i++) {
                leds_set(i, 0, 20, 0);
            }
            leds_refresh();
            vTaskDelay(pdMS_TO_TICKS(150));
            leds_clear();
            leds_refresh();
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        if (is_catcher) {
            // Step 5 gate (GAME.md section 7): confirm WiFi fits alongside
            // the always-on BLE stack before building the rest of the
            // selfie pipeline (captive DNS, HTTP server, HTML page). This
            // is a heap measurement only -- nothing here is the real
            // feature yet.
            char ssid[24];
            snprintf(ssid, sizeof(ssid), "BUGDEX-%04lx", (unsigned long)(bugdex_own_id() & 0xFFFF));
            display_show_text("Selfie: checking\nWiFi+BLE heap...");
            wifi_captive_start(ssid);
            vTaskDelay(pdMS_TO_TICKS(3000));
            wifi_captive_stop();
        }
    } else if (!peer_seen) {
        ESP_LOGI(TAG, "CATCH TIMEOUT waiting for peer result");
        display_show_text("Try again!\n(no response)");
    } else {
        ESP_LOGI(TAG, "CATCH FAILED ok=%d peer_ok=%d", ok, peer_ok);
        display_show_text("Try again!");
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    ble_proto_send_beacon(); // resume beaconing
    s_state = GAME_STATE_IDLE;
}

static void run_catch_as_catcher(uint32_t target_id)
{
    // Captured now, before the host pauses its BEACON for the encounter and
    // the radar reading goes stale -- this is what we're catching.
    uint8_t species = radar_host_species();

    uint32_t nonce = esp_random();
    ble_proto_send_catch_req(target_id, nonce);
    display_show_text("Catch request sent...\nwaiting for host");
    ESP_LOGI(TAG, "CATCHER: sent CATCH_REQ target=0x%08lx nonce=0x%08lx",
             (unsigned long)target_id, (unsigned long)nonce);

    int64_t deadline = esp_timer_get_time() + (int64_t)ACK_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        bugdex_event_t evt;
        if (ble_proto_recv(&evt, pdMS_TO_TICKS(50))
                && evt.type == BUGDEX_PKT_CATCH_ACK
                && evt.catch_ack.catcher_id == bugdex_own_id()
                && evt.catch_ack.nonce == nonce
                && evt.sender_id == target_id) {
            run_cue_and_result(target_id, nonce, evt.catch_ack.flash_delay_ms, true, species);
            return;
        }
    }

    ESP_LOGI(TAG, "CATCHER: no ACK, back to idle");
    display_show_text("No response.\nTry again!");
    vTaskDelay(pdMS_TO_TICKS(1200));
    ble_proto_send_beacon();
}

static void run_catch_as_host(uint32_t catcher_id, uint32_t nonce)
{
#if ANTI_FARMING_ENABLED
    // Anti-farming (GAME.md section 6): refuse to ACK if we caught (or were
    // caught by) this same badge within the last 30 minutes. This is the
    // host-side backstop; the catcher also checks its own met-log before
    // ever sending CATCH_REQ (see game_state_task).
    if (storage_recently_caught(catcher_id, now_seconds(), CATCH_COOLDOWN_SECONDS)) {
        ESP_LOGI(TAG, "HOST: refusing catcher=0x%08lx (caught within the last 30 min)",
                 (unsigned long)catcher_id);
        return; // no ACK -- the catcher's request simply times out
    }
#endif

    uint16_t flash_delay_ms = (uint16_t)(800 + (esp_random() % 1201)); // 800-2000ms
    ble_proto_send_catch_ack(catcher_id, nonce, flash_delay_ms);
    ESP_LOGI(TAG, "HOST: acked catcher=0x%08lx nonce=0x%08lx delay=%u",
             (unsigned long)catcher_id, (unsigned long)nonce, flash_delay_ms);

    run_cue_and_result(catcher_id, nonce, flash_delay_ms, false, 0);
}

static void update_idle_display(bool proximity_ok)
{
    static const char *stars[6] = { "-----", "*----", "**---", "***--", "****-", "*****" };
    int level = radar_level();
    int8_t rssi = radar_rssi();
    char text[112];

    if (level == 0) {
        snprintf(text, sizeof(text), "Bugdex (%d caught)\nID: %08lx  Sp: %u\nRadar: %s",
                 storage_dex_count(), (unsigned long)bugdex_own_id(), bugdex_own_species(), stars[level]);
    } else {
        snprintf(text, sizeof(text), "Bugdex (%d caught)\nID: %08lx  Sp: %u\nRadar: %s (%d dBm)%s",
                 storage_dex_count(), (unsigned long)bugdex_own_id(), bugdex_own_species(), stars[level], rssi,
                 proximity_ok ? "\nPress A to catch!" : "");
    }
    display_show_text(text);
}

static void game_state_task(void *arg)
{
    buttons_init();

    int idle_display_counter = 0;
    while (1) {
        buttons_poll();

        // Proximity bookkeeping runs every tick so the 2s hold works even
        // while the player isn't actively pressing anything.
        bool strong = (radar_host_id() != 0 && radar_rssi() >= CATCH_RSSI_THRESHOLD);
        if (strong) {
            if (s_strong_since_us == 0) {
                s_strong_since_us = esp_timer_get_time();
            }
        } else {
            s_strong_since_us = 0;
        }
        bool proximity_ok = strong && (esp_timer_get_time() - s_strong_since_us >= CATCH_RSSI_HOLD_US);

        for (int i = 0; i < BUTTON_COUNT; i++) {
            if (button_was_pressed((button_id_t)i)) {
                ESP_LOGI(TAG, "button %s: pressed", button_name((button_id_t)i));
            }
        }

        bugdex_event_t evt;
        if (ble_proto_recv(&evt, 0)) {
            if (evt.type == BUGDEX_PKT_CATCH_REQ && evt.catch_req.target_id == bugdex_own_id()
                    && evt.rssi >= CATCH_RSSI_THRESHOLD) {
                run_catch_as_host(evt.sender_id, evt.catch_req.nonce);
                idle_display_counter = 0;
                continue;
            }
            // stray/late packet while idle (e.g. a delayed CATCH_RESULT) -- ignore
        }

        if (button_was_pressed(BUTTON_A) && proximity_ok) {
            uint32_t target = radar_host_id();
#if ANTI_FARMING_ENABLED
            if (storage_recently_caught(target, now_seconds(), CATCH_COOLDOWN_SECONDS)) {
                display_show_text("Already caught,\ncome back later");
                vTaskDelay(pdMS_TO_TICKS(1500));
            } else
#endif
            {
                run_catch_as_catcher(target);
            }
            idle_display_counter = 0;
            continue;
        }

        if (++idle_display_counter >= 20) { // ~200ms at this 10ms tick
            idle_display_counter = 0;
            update_idle_display(proximity_ok);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void game_state_init(void)
{
    xTaskCreate(game_state_task, "game_state_task", 4096, NULL, 5, NULL);
}
