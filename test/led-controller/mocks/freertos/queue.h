#pragma once
/** @file queue.h @brief Host replacement for FreeRTOS queue.h; functions are FFF fakes (freertos_fakes.c). */
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length, UBaseType_t item_size, uint8_t *storage,
                                 StaticQueue_t *queue_buffer);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *higher_priority_task_woken);
BaseType_t xQueueOverwriteFromISR(QueueHandle_t queue, const void *item, BaseType_t *higher_priority_task_woken);
BaseType_t xQueueReset(QueueHandle_t queue);
#ifdef __cplusplus
}
#endif
