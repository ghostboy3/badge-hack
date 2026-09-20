#include "buttons.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define HC165_DATA_GPIO 7
#define HC165_LOAD_GPIO 20
#define HC165_CLK_GPIO  21
#define START_GPIO      9

#define DEBOUNCE_TICKS  3 // consecutive matching 10 ms polls before accepting a change

static const char *s_names[BUTTON_COUNT] = {
    "A", "B", "Home", "Down", "Left", "Right", "Up", "Aux1", "Start",
};

static bool s_debounced[BUTTON_COUNT];
static bool s_candidate[BUTTON_COUNT];
static int  s_stable_count[BUTTON_COUNT];
static bool s_pressed_edge[BUTTON_COUNT];
static bool s_released_edge[BUTTON_COUNT];

void buttons_init(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << HC165_LOAD_GPIO) | (1ULL << HC165_CLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << HC165_DATA_GPIO) | (1ULL << START_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in_conf);

    gpio_set_level(HC165_CLK_GPIO, 0);
    gpio_set_level(HC165_LOAD_GPIO, 1);
}

/* Shifts out all 8 active-low bits in A,B,Home,Down,Left,Right,Up,Aux1 order
 * (A first, Aux1 last) per custom-firmware-hal.md section 3. */
static void read_shift_register(bool active[8])
{
    gpio_set_level(HC165_LOAD_GPIO, 0);
    esp_rom_delay_us(1);
    gpio_set_level(HC165_LOAD_GPIO, 1);
    esp_rom_delay_us(1);

    for (int i = 0; i < 8; i++) {
        active[i] = (gpio_get_level(HC165_DATA_GPIO) == 0);
        gpio_set_level(HC165_CLK_GPIO, 1);
        esp_rom_delay_us(1);
        gpio_set_level(HC165_CLK_GPIO, 0);
        esp_rom_delay_us(1);
    }
}

void buttons_poll(void)
{
    bool raw[BUTTON_COUNT];
    read_shift_register(raw); // fills BUTTON_A .. BUTTON_AUX1
    raw[BUTTON_START] = (gpio_get_level(START_GPIO) == 0);

    for (int i = 0; i < BUTTON_COUNT; i++) {
        s_pressed_edge[i] = false;
        s_released_edge[i] = false;

        if (raw[i] == s_candidate[i]) {
            if (s_stable_count[i] < DEBOUNCE_TICKS) {
                s_stable_count[i]++;
            }
        } else {
            s_candidate[i] = raw[i];
            s_stable_count[i] = 0;
        }

        if (s_stable_count[i] >= DEBOUNCE_TICKS && s_candidate[i] != s_debounced[i]) {
            s_debounced[i] = s_candidate[i];
            if (s_debounced[i]) {
                s_pressed_edge[i] = true;
            } else {
                s_released_edge[i] = true;
            }
        }
    }
}

bool button_is_pressed(button_id_t id)
{
    return s_debounced[id];
}

bool button_was_pressed(button_id_t id)
{
    return s_pressed_edge[id];
}

bool button_was_released(button_id_t id)
{
    return s_released_edge[id];
}

const char *button_name(button_id_t id)
{
    return s_names[id];
}
