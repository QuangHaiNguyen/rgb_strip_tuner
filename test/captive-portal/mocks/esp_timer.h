#pragma once
/* Host mock of esp_timer.h: time comes from the fake clock in freertos_mock.c. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
