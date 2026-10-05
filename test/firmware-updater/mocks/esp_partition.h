#pragma once
/* Host mock of esp_partition.h: only what fw_meta_flash.c, fw_update.c and the updater use. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0x00, ESP_PARTITION_TYPE_DATA = 0x01 } esp_partition_type_t;
typedef int esp_partition_subtype_t;
#define ESP_PARTITION_SUBTYPE_APP_FACTORY (0x00)
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 (0x10)
#define ESP_PARTITION_SUBTYPE_DATA_OTA (0x00)
typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
} esp_partition_t;
#ifdef __cplusplus
extern "C" {
#endif
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size);
esp_err_t esp_partition_get_sha256(const esp_partition_t *partition, uint8_t *sha_256);
#ifdef __cplusplus
}
#endif
