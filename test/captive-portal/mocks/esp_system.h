#pragma once
/* Host mock of esp_system.h: only the heap queries used by the SPEC-005 FR-10 Debug line. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);
#ifdef __cplusplus
}
#endif
