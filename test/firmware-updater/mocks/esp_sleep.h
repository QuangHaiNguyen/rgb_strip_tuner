#pragma once
/* Host mock of esp_sleep.h. esp_deep_sleep_start() is not noreturn here: its fake either returns (a failed sleep)
 * or long-jumps back to the test harness. */
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
void esp_deep_sleep_start(void);
#ifdef __cplusplus
}
#endif
