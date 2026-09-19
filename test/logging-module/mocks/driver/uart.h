#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "fff.h"
#include "freertos/queue.h"

typedef int uart_port_t;

#define UART_NUM_0 (0)
#define UART_NUM_1 (1)
#define UART_NUM_2 (2)

typedef enum {
    UART_DATA_8_BITS = 3,
} uart_word_length_t;

typedef enum {
    UART_PARITY_DISABLE = 0,
} uart_parity_t;

typedef enum {
    UART_STOP_BITS_1 = 1,
} uart_stop_bits_t;

typedef enum {
    UART_HW_FLOWCTRL_DISABLE = 0,
} uart_hw_flowcontrol_t;

typedef enum {
    UART_SCLK_DEFAULT = 0,
} uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uart_sclk_t source_clk;
} uart_config_t;

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_FAKE_VALUE_FUNC(esp_err_t, uart_param_config, uart_port_t, const uart_config_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, uart_driver_install, uart_port_t, int, int, int, QueueHandle_t *,
                         int);
DECLARE_FAKE_VALUE_FUNC(int, uart_write_bytes, uart_port_t, const void *, size_t);

#ifdef __cplusplus
}
#endif
