#pragma once

#include <stdint.h>

#define LED_COUNT 6

void leds_init(void);
void leds_clear(void);
void leds_set(int index, uint8_t r, uint8_t g, uint8_t b);
void leds_refresh(void);
