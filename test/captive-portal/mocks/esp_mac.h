#pragma once
/* Host mock of esp_mac.h */
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA = 0, ESP_MAC_BASE = 100 } esp_mac_type_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
#ifdef __cplusplus
}
#endif
