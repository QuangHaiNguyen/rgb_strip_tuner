#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
#ifdef __cplusplus
}
#endif
