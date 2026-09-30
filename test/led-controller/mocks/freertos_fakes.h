#pragma once
/**
 * @file freertos_fakes.h
 * @brief FFF fakes of the FreeRTOS API used by led_controller.c and rmt_pulse_monitor.c.
 *
 * FreeRtosFakesReset() restores the defaults: every *CreateStatic() returns its buffer's address
 * as the handle, xTaskCreateStatic() returns its TCB address, take/give/send/overwrite (also FromISR)/reset
 * succeed, xQueueReceive() times out (pdFALSE) and uxSemaphoreGetCount() is 0.
 */
#include "fff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_FAKE_VALUE_FUNC(QueueHandle_t, xQueueCreateStatic, UBaseType_t, UBaseType_t, uint8_t *, StaticQueue_t *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueSend, QueueHandle_t, const void *, TickType_t);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueOverwrite, QueueHandle_t, const void *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueReceive, QueueHandle_t, void *, TickType_t);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueSendFromISR, QueueHandle_t, const void *, BaseType_t *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueOverwriteFromISR, QueueHandle_t, const void *, BaseType_t *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xQueueReset, QueueHandle_t);
DECLARE_FAKE_VALUE_FUNC(SemaphoreHandle_t, xSemaphoreCreateBinaryStatic, StaticSemaphore_t *);
DECLARE_FAKE_VALUE_FUNC(SemaphoreHandle_t, xSemaphoreCreateMutexStatic, StaticSemaphore_t *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xSemaphoreTake, SemaphoreHandle_t, TickType_t);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xSemaphoreGive, SemaphoreHandle_t);
DECLARE_FAKE_VALUE_FUNC(UBaseType_t, uxSemaphoreGetCount, SemaphoreHandle_t);
DECLARE_FAKE_VALUE_FUNC(TaskHandle_t, xTaskCreateStatic, TaskFunction_t, const char *, uint32_t, void *, UBaseType_t,
                        StackType_t *, StaticTask_t *);

/** Reset every FreeRTOS fake and install the default behavior described above. */
void FreeRtosFakesReset(void);

#ifdef __cplusplus
}
#endif
