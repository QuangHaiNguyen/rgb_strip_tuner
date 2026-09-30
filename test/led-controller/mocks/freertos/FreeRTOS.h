#pragma once
/**
 * @file FreeRTOS.h
 * @brief Host replacement for the FreeRTOS types used by led_controller and rmt_pulse_monitor.
 *
 * The tick rate is 100 Hz, the project's CONFIG_FREERTOS_HZ, and pdMS_TO_TICKS() truncates exactly
 * like the real macro, so tick arguments seen by the fakes match what the target would pass.
 * Every API function is an FFF fake (freertos_fakes.c); handles are just the addresses of the
 * static buffers given to the *CreateStatic() calls.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;

#define configTICK_RATE_HZ (100)
#define pdTRUE (1)
#define pdFALSE (0)
#define pdPASS (pdTRUE)
#define pdFAIL (pdFALSE)
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * (uint64_t)configTICK_RATE_HZ) / 1000U))

typedef struct { uint8_t opaque[80]; } StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;
typedef struct { uint8_t opaque[344]; } StaticTask_t;

typedef struct QueueDefinition *QueueHandle_t;
typedef QueueHandle_t SemaphoreHandle_t;
typedef struct tskTaskControlBlock *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
