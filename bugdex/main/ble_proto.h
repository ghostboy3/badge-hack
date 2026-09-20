#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define BUGDEX_SPECIES_COUNT 10

typedef enum {
    BUGDEX_PKT_BEACON       = 0x01,
    BUGDEX_PKT_CATCH_REQ    = 0x02,
    BUGDEX_PKT_CATCH_ACK    = 0x03,
    BUGDEX_PKT_CATCH_RESULT = 0x04,
    BUGDEX_PKT_SELFIE_OFFER = 0x05,
    BUGDEX_PKT_SELFIE_CHUNK = 0x06,
    BUGDEX_PKT_SELFIE_ACK   = 0x07,
} bugdex_pkt_type_t;

/* A decoded CATCH_* packet heard over the air (BEACON isn't queued here --
 * it only feeds the radar accessors below, see ble_proto_recv()). */
typedef struct {
    bugdex_pkt_type_t type;
    uint32_t sender_id;
    int8_t rssi;
    union {
        struct { uint32_t target_id; uint32_t nonce; } catch_req;
        struct { uint32_t catcher_id; uint32_t nonce; uint16_t flash_delay_ms; } catch_ack;
        struct { uint32_t nonce; uint8_t ok; uint16_t press_delta_ms; } catch_result;
    };
} bugdex_event_t;

/* Starts NimBLE (init once at boot, never deinit -- custom-firmware-hal.md
 * section 8), the extended-advertising instance (starts out beaconing),
 * and passive extended scanning. */
void ble_proto_init(void);

uint32_t bugdex_own_id(void);
uint8_t  bugdex_own_species(void);

/* Radar, per GAME.md section 6: smoothed RSSI of the strongest BEACON heard
 * recently from another badge, mapped to a 0-5 step level (0 = none/stale,
 * beacon not seen in the last 3s). */
int8_t   radar_rssi(void);
int      radar_level(void); // 0..5
uint32_t radar_host_id(void); // sender id of the tracked host, 0 if level is 0
uint8_t  radar_host_species(void); // species of the tracked host, 0 if level is 0

/* Pulls the next decoded CATCH_* event, waiting up to wait_ticks. Returns
 * false on timeout. */
bool ble_proto_recv(bugdex_event_t *out, TickType_t wait_ticks);

/* Re-announces this badge's BEACON (species, idle). Called to resume
 * beaconing after a CATCH_* packet has been advertised for a while --
 * GAME.md section 5: "advertised repeatedly ... stopped or replaced when
 * the next state begins." */
void ble_proto_send_beacon(void);

void ble_proto_send_catch_req(uint32_t target_id, uint32_t nonce);
void ble_proto_send_catch_ack(uint32_t catcher_id, uint32_t nonce, uint16_t flash_delay_ms);
void ble_proto_send_catch_result(uint32_t nonce, bool ok, uint16_t press_delta_ms);
