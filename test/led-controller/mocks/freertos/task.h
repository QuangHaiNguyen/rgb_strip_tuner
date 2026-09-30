#pragma once
/** @file task.h @brief Host replacement for FreeRTOS task.h; functions are FFF fakes (freertos_fakes.c). */
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskCreateStatic(TaskFunction_t task, const char *name, uint32_t stack_depth, void *arg,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb);
#ifdef __cplusplus
}
#endif
