#pragma once

#include <pthread.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

/**
 * @brief Host-test stand-in for FreeRTOS's static task control block.
 *
 * Backs the task with a real (detached) pthread, since the logging module's
 * writer task must genuinely run concurrently to drain the log queue.
 */
typedef struct StaticTask {
    pthread_t thread;
    TaskFunction_t func;
    void *arg;
} StaticTask_t;

typedef StaticTask_t *TaskHandle_t;

#ifdef __cplusplus
extern "C" {
#endif

TaskHandle_t xTaskCreateStatic(TaskFunction_t task_func, const char *name, uint32_t stack_depth,
                                void *params, UBaseType_t priority, StackType_t *stack_buffer,
                                StaticTask_t *task_buffer);

#ifdef __cplusplus
}
#endif
