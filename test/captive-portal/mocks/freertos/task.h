#pragma once
#include "FreeRTOS.h"
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted } eTaskState;
#ifdef __cplusplus
extern "C" {
#endif
TaskHandle_t xTaskCreateStatic(TaskFunction_t task, const char *name, uint32_t stack_depth, void *arg,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t *previous_wake_ticks, TickType_t increment_ticks);
TickType_t xTaskGetTickCount(void);
eTaskState eTaskGetState(TaskHandle_t task);
#ifdef __cplusplus
}
#endif
