#pragma once
#include <cstdint>
#include "esp_timer.h"
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
constexpr int GPIO_PULLUP_DISABLE = 0;
constexpr int GPIO_PULLDOWN_DISABLE = 0;
constexpr int GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
    uint64_t pin_bit_mask = 0;
    int mode = 0;
    int pull_up_en = 0;
    int pull_down_en = 0;
    int intr_type = 0;
};
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
esp_err_t gpio_config(const gpio_config_t*);
