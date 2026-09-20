#pragma once

#include <stdbool.h>

/* Shift order out of the 74HC165 (custom-firmware-hal.md section 3):
 * A shifts out first, Aux1 last. Start is a separate dedicated GPIO. */
typedef enum {
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_HOME,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_UP,
    BUTTON_AUX1,
    BUTTON_START,
    BUTTON_COUNT,
} button_id_t;

void buttons_init(void);

/* Reads the shift register + Start GPIO once and advances the debounce
 * state for every button. Call at a steady ~10 ms cadence from one task. */
void buttons_poll(void);

bool button_is_pressed(button_id_t id);   // current debounced level
bool button_was_pressed(button_id_t id);  // rising edge on the last poll
bool button_was_released(button_id_t id); // falling edge on the last poll

const char *button_name(button_id_t id);
