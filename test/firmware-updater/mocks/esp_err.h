#pragma once
/* Host mock of esp_err.h for SPEC-007: the captive-portal codes plus the ones fw_meta, fw_update and the updater use
 * (values as in ESP-IDF). */
#include "../../captive-portal/mocks/esp_err.h"
#define ESP_ERR_INVALID_CRC (0x109)
#define ESP_ERR_OTA_BASE (0x1500)
#define ESP_ERR_OTA_VALIDATE_FAILED (ESP_ERR_OTA_BASE + 0x03)
