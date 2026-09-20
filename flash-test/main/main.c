/*
 * "did the re-flash actually take" test, now with text on the ST7789, shake
 * detection on the accelerometer, and a phone-camera-to-badge-screen path.
 *
 * - Chases a single dim red pixel around the 6 edge WS2812 LEDs (GPIO3),
 *   or flashes them all green while the badge is being shaken.
 * - Polls the SC7A20 accelerometer (I2C `0x19`) and flags a shake whenever
 *   total acceleration deviates far enough from a resting 1 g.
 * - Prints a heartbeat line over USB-Serial-JTAG every 300 ms.
 * - Draws "Hello, badge!" on the display via esp_lcd + LVGL.
 * - Flipping the Aux1 switch (74HC165 shift register) starts a WiFi hotspot
 *   + web page; taking a photo there sends it to the badge and displays it
 *   full-screen. Flipping Aux1 back off tears the hotspot down.
 *
 * If you see the LED chase, the heartbeat in `idf.py monitor`, and the text
 * on screen, your custom firmware is running on the badge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_http_server.h"

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
#define PHOTO_BYTES  (LCD_H_RES * LCD_V_RES * 2) // RGB565

#define I2C_PIN_SDA  5
#define I2C_PIN_SCL  6
#define ACCEL_I2C_ADDR    0x19
#define ACCEL_REG_WHO_AM_I 0x0F
#define ACCEL_REG_CTRL1    0x20
#define ACCEL_REG_CTRL4    0x23
#define ACCEL_REG_STATUS   0x27
#define ACCEL_REG_OUT_X_L  0x28
#define ACCEL_WHO_AM_I_VAL 0x11
#define ACCEL_AUTOINCREMENT_BIT 0x80

/* Total acceleration sits at ~1000 mg when the badge is still, regardless of
 * orientation -- only genuine motion (shaking) pushes the vector magnitude
 * away from that, so this is a tilt-independent shake detector. */
#define SHAKE_DEVIATION_MG  350
#define SHAKE_HOLD_TICKS    30 // ~300 ms at the 10 ms poll rate below

#define HC165_DATA_GPIO 7
#define HC165_LOAD_GPIO 20
#define HC165_CLK_GPIO  21
#define BUTTON_DEBOUNCE_TICKS 3 // consecutive matching 50 ms polls before acting

#define CAM_WIFI_SSID     "Badge-Cam"
#define CAM_WIFI_PASSWORD "badge1234"
#define CAM_WIFI_CHANNEL  1

static const char *TAG = "flash_test";

static volatile bool s_shake_active = false;

static lv_obj_t *s_status_label = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static httpd_handle_t s_httpd = NULL;
static bool s_wifi_ready = false;   // esp_wifi_init() etc. done once at boot
static bool s_lvgl_paused = false;  // true once a raw photo has overwritten the LVGL screen

extern const uint8_t camera_html_start[] asm("_binary_camera_html_start");
extern const uint8_t camera_html_end[]   asm("_binary_camera_html_end");

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
        bool shaking = s_shake_active;

        led_strip_clear(strip);
        if (shaking) {
            for (int i = 0; i < LED_COUNT; i++) {
                led_strip_set_pixel(strip, i, 0, 24, 0); // dim green -- shake flash
            }
        } else {
            led_strip_set_pixel(strip, pos, 16, 0, 0); // dim red -- idle chase
        }
        led_strip_refresh(strip);

        ESP_LOGI(TAG, "heartbeat tick=%lu led=%d shake=%d -- re-flash works!",
                 (unsigned long)tick, pos, shaking);

        pos = (pos + 1) % LED_COUNT;
        tick++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void accel_task(void *arg)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = I2C_PIN_SDA,
        .scl_io_num = I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t accel;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &accel));

    /* Bounded timeouts everywhere -- the NFC reader shares this bus and can
     * wedge it on droopy battery power, so we never block forever. */
    uint8_t who_am_i_reg = ACCEL_REG_WHO_AM_I;
    uint8_t who_am_i = 0;
    esp_err_t err = i2c_master_transmit_receive(accel, &who_am_i_reg, 1, &who_am_i, 1, 100);
    if (err != ESP_OK || who_am_i != ACCEL_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "accel WHO_AM_I check failed (err=%d, got=0x%02x) -- skipping shake detection",
                  err, who_am_i);
        vTaskDelete(NULL);
        return;
    }

    uint8_t ctrl1[2] = { ACCEL_REG_CTRL1, 0x57 }; // 100 Hz, all axes on
    ESP_ERROR_CHECK(i2c_master_transmit(accel, ctrl1, sizeof(ctrl1), 100));
    uint8_t ctrl4[2] = { ACCEL_REG_CTRL4, 0x80 }; // BDU, little-endian, +/-2g
    ESP_ERROR_CHECK(i2c_master_transmit(accel, ctrl4, sizeof(ctrl4), 100));

    int shake_hold = 0;

    while (1) {
        uint8_t status_reg = ACCEL_REG_STATUS;
        uint8_t status = 0;
        if (i2c_master_transmit_receive(accel, &status_reg, 1, &status, 1, 50) == ESP_OK
                && (status & (1 << 3))) { // ZYXDA
            uint8_t out_reg = ACCEL_REG_OUT_X_L | ACCEL_AUTOINCREMENT_BIT;
            uint8_t raw[6];
            if (i2c_master_transmit_receive(accel, &out_reg, 1, raw, sizeof(raw), 50) == ESP_OK) {
                int16_t x = ((int16_t)((raw[1] << 8) | raw[0])) >> 4;
                int16_t y = ((int16_t)((raw[3] << 8) | raw[2])) >> 4;
                int16_t z = ((int16_t)((raw[5] << 8) | raw[4])) >> 4;

                float magnitude_mg = sqrtf((float)x * x + (float)y * y + (float)z * z);
                float deviation = fabsf(magnitude_mg - 1000.0f);

                if (deviation > SHAKE_DEVIATION_MG) {
                    shake_hold = SHAKE_HOLD_TICKS;
                }
            }
        }

        if (shake_hold > 0) {
            shake_hold--;
        }
        s_shake_active = (shake_hold > 0);

        vTaskDelay(pdMS_TO_TICKS(10));
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

    s_panel_handle = panel_handle;

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

static void set_status_label(const char *text)
{
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_status_label, text);
        lvgl_port_unlock();
    }
}

/* WiFi + the HTTP server + LVGL's own buffers leave nowhere near enough free
 * heap for a 150KB full-frame buffer (measured ~32KB free in practice), so
 * the frame is streamed straight to the panel in small row-chunks instead of
 * buffered whole. A static buffer is used (plain internal DRAM is already
 * DMA-capable on the ESP32-C3) so there's no per-photo allocation at all. */
#define PHOTO_CHUNK_ROWS  16 // 240 / 16 = 15 chunks, no remainder
#define PHOTO_CHUNK_BYTES (LCD_H_RES * PHOTO_CHUNK_ROWS * 2)
static uint8_t s_photo_chunk[PHOTO_CHUNK_BYTES];

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)camera_html_start,
                            camera_html_end - camera_html_start);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    if (req->content_len != PHOTO_BYTES) {
        ESP_LOGE(TAG, "upload size mismatch: got %d, want %d", (int)req->content_len, PHOTO_BYTES);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a raw 320x240 RGB565 frame");
        return ESP_FAIL;
    }

    /* Bypasses LVGL for the duration of photo mode -- it stays paused (the
     * photo stays on screen) until photo mode is turned off. */
    lvgl_port_stop();
    s_lvgl_paused = true;

    int y = 0;
    while (y < LCD_V_RES) {
        size_t received_in_chunk = 0;
        while (received_in_chunk < PHOTO_CHUNK_BYTES) {
            int r = httpd_req_recv(req, (char *)s_photo_chunk + received_in_chunk,
                                    PHOTO_CHUNK_BYTES - received_in_chunk);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (r <= 0) {
                ESP_LOGE(TAG, "upload recv failed (r=%d) at row %d", r, y);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            received_in_chunk += r;
        }

        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LCD_H_RES, y + PHOTO_CHUNK_ROWS,
                                                   s_photo_chunk));
        /* Let the SPI DMA drain before the next network read overwrites this
         * buffer -- draw_bitmap queues the transfer and can return before it
         * physically completes. */
        vTaskDelay(pdMS_TO_TICKS(5));

        y += PHOTO_CHUNK_ROWS;
    }

    ESP_LOGI(TAG, "photo displayed");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void wifi_init_once(void)
{
    if (s_wifi_ready) {
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(CAM_WIFI_SSID),
            .channel = CAM_WIFI_CHANNEL,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, CAM_WIFI_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, CAM_WIFI_PASSWORD, sizeof(wifi_config.ap.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    s_wifi_ready = true;
}

static void start_photo_mode(void)
{
    ESP_LOGI(TAG, "photo mode ON (free heap: %u bytes)", (unsigned)esp_get_free_heap_size());

    wifi_init_once();
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t upload_uri = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler };
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &upload_uri);

    char msg[128];
    snprintf(msg, sizeof(msg), "Photo mode\nWiFi: %s\nPass: %s\nhttp://192.168.4.1/",
             CAM_WIFI_SSID, CAM_WIFI_PASSWORD);
    set_status_label(msg);
}

static void stop_photo_mode(void)
{
    ESP_LOGI(TAG, "photo mode OFF");

    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();

    if (s_lvgl_paused) {
        lvgl_port_resume();
        s_lvgl_paused = false;

        /* A raw photo was blitted straight to the panel while LVGL was
         * paused, bypassing its dirty-rect tracking -- LVGL only knows to
         * redraw the label's own area otherwise, leaving stale photo pixels
         * around it. Force it to repaint the whole screen. */
        if (lvgl_port_lock(0)) {
            lv_obj_invalidate(lv_screen_active());
            lvgl_port_unlock();
        }
    }
    set_status_label("Hello, badge!");
}

static void hc165_init(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << HC165_LOAD_GPIO) | (1ULL << HC165_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << HC165_DATA_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in_conf);

    gpio_set_level(HC165_CLK_GPIO, 0);
    gpio_set_level(HC165_LOAD_GPIO, 1);
}

/* Aux1 is a maintained side switch (doc section 3), shifted out 8th/last,
 * active-low. Returns true while the switch is in its "on" position. */
static bool read_aux1_active(void)
{
    gpio_set_level(HC165_LOAD_GPIO, 0);
    esp_rom_delay_us(1);
    gpio_set_level(HC165_LOAD_GPIO, 1);
    esp_rom_delay_us(1);

    bool aux1_active = false;
    for (int i = 0; i < 8; i++) {
        if (i == 7) {
            aux1_active = (gpio_get_level(HC165_DATA_GPIO) == 0);
        }
        gpio_set_level(HC165_CLK_GPIO, 1);
        esp_rom_delay_us(1);
        gpio_set_level(HC165_CLK_GPIO, 0);
        esp_rom_delay_us(1);
    }
    return aux1_active;
}

static void photo_mode_task(void *arg)
{
    hc165_init();

    bool photo_mode_on = false;
    bool candidate = false;
    int stable_count = 0;

    while (1) {
        bool raw = read_aux1_active();

        if (raw == candidate) {
            if (stable_count < BUTTON_DEBOUNCE_TICKS) {
                stable_count++;
            }
        } else {
            candidate = raw;
            stable_count = 0;
        }

        if (stable_count >= BUTTON_DEBOUNCE_TICKS && candidate != photo_mode_on) {
            photo_mode_on = candidate;
            if (photo_mode_on) {
                start_photo_mode();
            } else {
                stop_photo_mode();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Custom firmware booted -- flash test starting");

    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);
    xTaskCreate(accel_task, "accel_task", 4096, NULL, 5, NULL);
    xTaskCreate(photo_mode_task, "photo_mode_task", 4096, NULL, 5, NULL);

    lv_display_t *disp = display_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "display init failed -- skipping text");
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

        s_status_label = lv_label_create(screen);
        lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
        lv_label_set_text(s_status_label, "Hello, badge!");
        lv_obj_center(s_status_label);
        lvgl_port_unlock();
    }
}
