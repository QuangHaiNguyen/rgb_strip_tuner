/* FFF fakes of the FreeRTOS API; see freertos_fakes.h. FFF globals live in log_fakes.c. */
#include "freertos_fakes.h"

DEFINE_FAKE_VALUE_FUNC(QueueHandle_t, xQueueCreateStatic, UBaseType_t, UBaseType_t, uint8_t *, StaticQueue_t *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueSend, QueueHandle_t, const void *, TickType_t);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueOverwrite, QueueHandle_t, const void *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueReceive, QueueHandle_t, void *, TickType_t);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueSendFromISR, QueueHandle_t, const void *, BaseType_t *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueOverwriteFromISR, QueueHandle_t, const void *, BaseType_t *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xQueueReset, QueueHandle_t);
DEFINE_FAKE_VALUE_FUNC(SemaphoreHandle_t, xSemaphoreCreateBinaryStatic, StaticSemaphore_t *);
DEFINE_FAKE_VALUE_FUNC(SemaphoreHandle_t, xSemaphoreCreateMutexStatic, StaticSemaphore_t *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xSemaphoreTake, SemaphoreHandle_t, TickType_t);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xSemaphoreGive, SemaphoreHandle_t);
DEFINE_FAKE_VALUE_FUNC(UBaseType_t, uxSemaphoreGetCount, SemaphoreHandle_t);
DEFINE_FAKE_VALUE_FUNC(TaskHandle_t, xTaskCreateStatic, TaskFunction_t, const char *, uint32_t, void *, UBaseType_t,
                       StackType_t *, StaticTask_t *);

static QueueHandle_t CreateQueueFake(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *buffer)
{
    (void)length;
    (void)item_size;
    (void)storage;
    return (QueueHandle_t)buffer;
}

static SemaphoreHandle_t CreateSemaphoreFake(StaticSemaphore_t *buffer)
{
    return (SemaphoreHandle_t)buffer;
}

static TaskHandle_t CreateTaskFake(TaskFunction_t task, const char *name, uint32_t stack_depth, void *arg,
                                   UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb)
{
    (void)task;
    (void)name;
    (void)stack_depth;
    (void)arg;
    (void)priority;
    (void)stack;
    return (TaskHandle_t)tcb;
}

void FreeRtosFakesReset(void)
{
    RESET_FAKE(xQueueCreateStatic);
    RESET_FAKE(xQueueSend);
    RESET_FAKE(xQueueOverwrite);
    RESET_FAKE(xQueueReceive);
    RESET_FAKE(xQueueSendFromISR);
    RESET_FAKE(xQueueOverwriteFromISR);
    RESET_FAKE(xQueueReset);
    RESET_FAKE(xSemaphoreCreateBinaryStatic);
    RESET_FAKE(xSemaphoreCreateMutexStatic);
    RESET_FAKE(xSemaphoreTake);
    RESET_FAKE(xSemaphoreGive);
    RESET_FAKE(uxSemaphoreGetCount);
    RESET_FAKE(xTaskCreateStatic);

    xQueueCreateStatic_fake.custom_fake = CreateQueueFake;
    xSemaphoreCreateBinaryStatic_fake.custom_fake = CreateSemaphoreFake;
    xSemaphoreCreateMutexStatic_fake.custom_fake = CreateSemaphoreFake;
    xTaskCreateStatic_fake.custom_fake = CreateTaskFake;
    xQueueSend_fake.return_val = pdTRUE;
    xQueueOverwrite_fake.return_val = pdPASS;
    xQueueReceive_fake.return_val = pdFALSE;
    xQueueSendFromISR_fake.return_val = pdTRUE;
    xQueueOverwriteFromISR_fake.return_val = pdPASS;   /* always succeeds on a 1-deep queue */
    xQueueReset_fake.return_val = pdPASS;
    xSemaphoreTake_fake.return_val = pdTRUE;
    xSemaphoreGive_fake.return_val = pdTRUE;
    uxSemaphoreGetCount_fake.return_val = 0;
}
