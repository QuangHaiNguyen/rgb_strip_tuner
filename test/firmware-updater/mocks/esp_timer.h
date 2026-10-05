#pragma once
/* Host mock of esp_timer.h for the SPEC-007 binaries: esp_timer_get_time() (as test/captive-portal/mocks/esp_timer.h)
 * plus the one-shot timer API used by fw_update.c's FR-11 fallback (StartFwHealthyTimer(), 2026-10-05). The timer
 * functions are FFF fakes in fw_flash_fakes.c. */
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
} esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *create_args, esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
#ifdef __cplusplus
}
#endif
