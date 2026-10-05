#pragma once
/* Host mock of esp_system.h for SPEC-007: the captive-portal heap queries plus esp_reset_reason_t (values as in
 * ESP-IDF v6.0), esp_reset_reason() and esp_restart(). */
#include "../../captive-portal/mocks/esp_system.h"
typedef enum {
    ESP_RST_UNKNOWN = 0,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);
#ifdef __cplusplus
}
#endif
