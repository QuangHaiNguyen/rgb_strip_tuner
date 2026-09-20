#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks);
#ifdef __cplusplus
}
#endif
