#include "display.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#define LCD_HOST     SPI2_HOST
#define LCD_PIN_MOSI 10
#define LCD_PIN_CLK  1
#define LCD_PIN_CS   2
#define LCD_PIN_DC   0
#define LCD_PIN_RST  4
#define LCD_H_RES    320
#define LCD_V_RES    240

static const char *TAG = "display";
static lv_obj_t *s_label = NULL;

lv_display_t *display_init(void)
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

    /* swap_xy/mirror MUST be set here, not as standalone esp_lcd_panel_*
     * calls before this -- lvgl_port_add_disp() applies its own rotation
     * state on top of the panel during init and silently overwrites any
     * orientation set beforehand (found the hard way in flash-test). */
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

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

        s_label = lv_label_create(screen);
        lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(s_label, &lv_font_montserrat_20, 0);
        // Constrained width + wrap so the larger font can't run text off
        // the edge of the screen -- it wraps to more lines instead.
        lv_obj_set_width(s_label, LCD_H_RES - 20);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_label);
        lvgl_port_unlock();
    }

    return disp;
}

void display_show_text(const char *text)
{
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_label, text);
        lvgl_port_unlock();
    }
}
