/* FFF fakes for the hardware-facing calls the logging module depends on. */

#include "fff.h"

DEFINE_FFF_GLOBALS;

#include "driver/uart.h"
#include "esp_timer.h"

DEFINE_FAKE_VALUE_FUNC(esp_err_t, uart_param_config, uart_port_t, const uart_config_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, uart_driver_install, uart_port_t, int, int, int, QueueHandle_t *,
                        int);
DEFINE_FAKE_VALUE_FUNC(int, uart_write_bytes, uart_port_t, const void *, size_t);
DEFINE_FAKE_VALUE_FUNC(int64_t, esp_timer_get_time);
