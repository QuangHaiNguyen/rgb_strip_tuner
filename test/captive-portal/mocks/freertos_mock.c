/* Deterministic FreeRTOS simulation; see freertos_mock.h. */
#include "freertos_mock.h"
#include <string.h>
#include <ucontext.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MAX_TASKS (4)
#define TASK_STACK_BYTES (256 * 1024)

typedef struct {
    TaskFunction_t entry;
    void *arg;
    char name[32];
    ucontext_t context;
    bool is_started;
    bool is_deleted;
    char stack[TASK_STACK_BYTES];
} mock_task_t;

static mock_task_t s_tasks[MAX_TASKS];
static int s_task_count;
static int s_current = -1;
static ucontext_t s_main_context;
static uint32_t s_now_ms;
static uint32_t s_run_until_ms;
static int s_mutex_balance;

void MockFreeRtosReset(void)
{
    memset(s_tasks, 0, sizeof(s_tasks));
    s_task_count = 0;
    s_current = -1;
    s_now_ms = 0;
    s_run_until_ms = 0;
    s_mutex_balance = 0;
}

uint32_t MockGetNowMs(void) { return s_now_ms; }
int MockGetTaskCount(void) { return s_task_count; }
const char *MockGetTaskName(int index) { return s_tasks[index].name; }
int MockGetMutexBalance(void) { return s_mutex_balance; }
void MockMarkTaskDeleted(int index) { s_tasks[index].is_deleted = true; }
int64_t esp_timer_get_time(void) { return (int64_t)s_now_ms * 1000; }

static void YieldToMain(void)
{
    swapcontext(&s_tasks[s_current].context, &s_main_context);
}

/** Block the caller until absolute time @p target_ms; a task yields to the test when the run window ends. */
static void SleepUntil(uint32_t target_ms)
{
    if (s_current >= 0) {
        while (target_ms > s_run_until_ms) {
            s_now_ms = s_run_until_ms;
            YieldToMain();
        }
    }
    if (target_ms > s_now_ms) {
        s_now_ms = target_ms;
    }
}

static void TaskTrampoline(void)
{
    mock_task_t *task = &s_tasks[s_current];
    task->entry(task->arg);
    task->is_deleted = true;
    swapcontext(&task->context, &s_main_context);
}

void MockRunTask(int index, uint32_t until_ms)
{
    mock_task_t *task = &s_tasks[index];
    if (task->is_deleted) {
        return;
    }
    s_run_until_ms = until_ms;
    s_current = index;
    if (!task->is_started) {
        task->is_started = true;
        getcontext(&task->context);
        task->context.uc_stack.ss_sp = task->stack;
        task->context.uc_stack.ss_size = sizeof(task->stack);
        task->context.uc_link = &s_main_context;
        makecontext(&task->context, TaskTrampoline, 0);
    }
    swapcontext(&s_main_context, &task->context);
    s_current = -1;
    if (s_now_ms < until_ms && !task->is_deleted) {
        s_now_ms = until_ms;
    }
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t task, const char *name, uint32_t stack_depth, void *arg,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb)
{
    (void)stack_depth; (void)priority; (void)stack; (void)tcb;
    if (s_task_count >= MAX_TASKS) {
        return NULL;
    }
    mock_task_t *slot = &s_tasks[s_task_count++];
    slot->entry = task;
    slot->arg = arg;
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    return slot;
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL && s_current >= 0) {
        s_tasks[s_current].is_deleted = true;
        for (;;) {
            YieldToMain();
        }
    }
    if (task != NULL) {
        ((mock_task_t *)task)->is_deleted = true;
    }
}

void vTaskDelay(TickType_t ticks) { SleepUntil(s_now_ms + ticks); }

void vTaskDelayUntil(TickType_t *previous_wake_ticks, TickType_t increment_ticks)
{
    *previous_wake_ticks += increment_ticks;
    SleepUntil(*previous_wake_ticks);
}

TickType_t xTaskGetTickCount(void) { return s_now_ms; }

eTaskState eTaskGetState(TaskHandle_t task)
{
    return ((mock_task_t *)task)->is_deleted ? eDeleted : eRunning;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *queue)
{
    queue->length = length;
    queue->item_size = item_size;
    queue->head = 0;
    queue->count = 0;
    queue->storage = storage;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    (void)wait_ticks;
    if (queue->count >= queue->length) {
        return pdFALSE;
    }
    memcpy(queue->storage + ((queue->head + queue->count) % queue->length) * queue->item_size, item, queue->item_size);
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    uint64_t deadline_ms = (wait_ticks == portMAX_DELAY) ? UINT64_MAX : (uint64_t)s_now_ms + wait_ticks;
    for (;;) {
        if (queue->count > 0) {
            memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
            queue->head = (queue->head + 1) % queue->length;
            queue->count--;
            return pdTRUE;
        }
        if (s_current < 0 || deadline_ms <= s_now_ms) {
            return pdFALSE;
        }
        if (deadline_ms > s_run_until_ms) {
            s_now_ms = s_run_until_ms;
            YieldToMain();  /* the test may post a message before resuming us */
            continue;
        }
        s_now_ms = (uint32_t)deadline_ms;
        return pdFALSE;
    }
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    buffer->take_count = 0;
    buffer->give_count = 0;
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks)
{
    (void)wait_ticks;
    semaphore->take_count++;
    s_mutex_balance++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->give_count++;
    s_mutex_balance--;
    return pdTRUE;
}
