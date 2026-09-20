#pragma once

typedef enum {
    GAME_STATE_IDLE,
    GAME_STATE_CATCH,
} game_state_t;

/* Owns button polling and drives the whole state machine (GAME.md section
 * 6) in one task -- catch timing needs first-hand, low-latency access to
 * button edges, so this does not go through a separate polling task. */
void game_state_init(void);

game_state_t game_state_current(void);
