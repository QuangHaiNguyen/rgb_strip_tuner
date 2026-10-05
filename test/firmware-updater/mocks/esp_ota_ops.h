#pragma once
/* Host mock of esp_ota_ops.h: the OTA calls of the updater upload, plus esp_ota_set_boot_partition() so a test can
 * assert that nothing writes otadata (SPEC-007 FR-4, FR-8). */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
typedef uint32_t esp_ota_handle_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
#ifdef __cplusplus
}
#endif
