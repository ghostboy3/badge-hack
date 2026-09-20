#include "leds.h"

#include "esp_err.h"
#include "led_strip.h"

#define LED_GPIO 3

static led_strip_handle_t s_strip;

void leds_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
}

void leds_clear(void)
{
    led_strip_clear(s_strip);
}

void leds_set(int index, uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, index, r, g, b);
}

void leds_refresh(void)
{
    led_strip_refresh(s_strip);
}
