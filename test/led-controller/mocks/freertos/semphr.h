#pragma once
/** @file semphr.h @brief Host replacement for FreeRTOS semphr.h; functions are FFF fakes (freertos_fakes.c). */
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t semaphore);
#ifdef __cplusplus
}
#endif
