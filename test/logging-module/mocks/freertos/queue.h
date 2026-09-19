#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

/**
 * @brief Host-test stand-in for FreeRTOS's static queue control block.
 *
 * Unlike real FreeRTOS (where StaticQueue_t is opaque, fixed-size storage),
 * this mock defines the real fields directly, since on the host we implement
 * genuine ring-buffer + mutex semantics rather than faking the behavior.
 */
typedef struct StaticQueue {
    uint8_t *storage;
    UBaseType_t item_size;
    UBaseType_t capacity;
    size_t head;
    size_t count;
    pthread_mutex_t lock;
} StaticQueue_t;

typedef StaticQueue_t *QueueHandle_t;

#ifdef __cplusplus
extern "C" {
#endif

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length, UBaseType_t item_size,
                                  uint8_t *storage_buffer, StaticQueue_t *queue_struct);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out_item, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
