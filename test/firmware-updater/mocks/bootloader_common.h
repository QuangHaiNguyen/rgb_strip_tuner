#pragma once
/* Host mock of bootloader_common.h: the RTC retain-memory calls of the FR-40 jump. */
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;
#ifdef __cplusplus
extern "C" {
#endif
void bootloader_common_update_rtc_retain_mem(esp_partition_pos_t *partition, bool reboot_counter);
esp_partition_pos_t *bootloader_common_get_rtc_retain_mem_partition(void);
#ifdef __cplusplus
}
#endif
