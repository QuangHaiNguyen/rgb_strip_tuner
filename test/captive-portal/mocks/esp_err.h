#pragma once
/* Host mock of esp_err.h */
typedef int esp_err_t;
#define ESP_OK (0)
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM (0x101)
#define ESP_ERR_INVALID_ARG (0x102)
#define ESP_ERR_INVALID_STATE (0x103)
#define ESP_ERR_NOT_FOUND (0x105)
#define ESP_ERR_NVS_NOT_FOUND (0x1102)
#define ESP_ERR_NVS_INVALID_LENGTH (0x1106)
#define ESP_ERR_NVS_NO_FREE_PAGES (0x110d)
#define ESP_ERR_WIFI_STATE (0x300b)
#ifdef __cplusplus
extern "C" {
#endif
const char *esp_err_to_name(esp_err_t code);
#ifdef __cplusplus
}
#endif
