#pragma once
/* Host mock of driver/gpio.h (only what button.c uses). */
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;
typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gpio_config(const gpio_config_t *config);
int gpio_get_level(gpio_num_t gpio_num);
#ifdef __cplusplus
}
#endif
