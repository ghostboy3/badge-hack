/*
 * "did the re-flash actually take" test, now with text on the ST7789.
 *
 * - Chases a single dim red pixel around the 6 edge WS2812 LEDs (GPIO3).
 * - Prints a heartbeat line over USB-Serial-JTAG every 300 ms.
 * - Draws "Hello, badge!" on the display via esp_lcd + LVGL.
 *
 * If you see the LED chase, the heartbeat in `idf.py monitor`, and the text
 * on screen, your custom firmware is running on the badge.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "led_strip.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#define LED_GPIO   3
#define LED_COUNT  6

#define LCD_HOST     SPI2_HOST
#define LCD_PIN_MOSI 10
#define LCD_PIN_CLK  1
#define LCD_PIN_CS   2
#define LCD_PIN_DC   0
#define LCD_PIN_RST  4
#define LCD_H_RES    320
#define LCD_V_RES    240

static const char *TAG = "flash_test";

static void led_task(void *arg)
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

    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    led_strip_clear(strip);

    int pos = 0;
    uint32_t tick = 0;

    while (1) {
        led_strip_clear(strip);
        led_strip_set_pixel(strip, pos, 16, 0, 0); // dim red -- stay gentle on AA power
        led_strip_refresh(strip);

        ESP_LOGI(TAG, "heartbeat tick=%lu led=%d -- re-flash works!", (unsigned long)tick, pos);

        pos = (pos + 1) % LED_COUNT;
        tick++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static lv_display_t *display_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* swap_xy/mirror belong here, not as standalone esp_lcd_panel_* calls --
     * lvgl_port_add_disp() applies its own rotation state on top of the panel
     * during init and will silently overwrite any orientation set beforehand. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };

    return lvgl_port_add_disp(&disp_cfg);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Custom firmware booted -- flash test starting");

    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);

    lv_display_t *disp = display_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed -- skipping text");
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

        lv_obj_t *label = lv_label_create(screen);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, "Hello, badge!");
        lv_obj_center(label);
        lvgl_port_unlock();
    }
}
