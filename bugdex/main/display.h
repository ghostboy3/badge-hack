#pragma once

#include "lvgl.h"

/* Brings up the ST7789 (SPI2) + LVGL. Returns NULL on failure. */
lv_display_t *display_init(void);

/* HAL bring-up smoke test: black screen, centered white text. */
void display_show_text(const char *text);
