#pragma once
/* Host mock of FreeRTOS types. One tick is one millisecond. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;

#define pdTRUE (1)
#define pdFALSE (0)
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFUL)
#define portTICK_PERIOD_MS (1U)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

typedef struct StaticQueue {
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t head;
    UBaseType_t count;
    uint8_t *storage;
} StaticQueue_t;
typedef struct { int unused; } StaticTask_t;
typedef struct { int take_count; int give_count; } StaticSemaphore_t;

typedef StaticQueue_t *QueueHandle_t;
typedef void *TaskHandle_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
