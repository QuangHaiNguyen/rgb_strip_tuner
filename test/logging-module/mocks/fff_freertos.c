/* Real, minimal FreeRTOS queue/task semantics for host testing, backed by
 * pthreads. Not FFF-faked: the logging module's queue/writer-task behavior
 * (drop-oldest-on-full, non-blocking send, background draining) must
 * genuinely execute for the tests to be meaningful. */

#include "freertos/queue.h"
#include "freertos/task.h"

#include <string.h>
#include <unistd.h>

QueueHandle_t xQueueCreateStatic(UBaseType_t queue_length, UBaseType_t item_size,
                                  uint8_t *storage_buffer, StaticQueue_t *queue_struct)
{
    queue_struct->storage = storage_buffer;
    queue_struct->item_size = item_size;
    queue_struct->capacity = queue_length;
    queue_struct->head = 0;
    queue_struct->count = 0;
    pthread_mutex_init(&queue_struct->lock, NULL);
    return queue_struct;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait; /* Only ever called with 0 (non-blocking) by logging.c. */

    pthread_mutex_lock(&queue->lock);
    if (queue->count >= queue->capacity) {
        pthread_mutex_unlock(&queue->lock);
        return pdFALSE;
    }
    size_t tail = (queue->head + queue->count) % queue->capacity;
    memcpy(queue->storage + (tail * queue->item_size), item, queue->item_size);
    queue->count++;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *out_item, TickType_t ticks_to_wait)
{
    for (;;) {
        pthread_mutex_lock(&queue->lock);
        if (queue->count > 0) {
            memcpy(out_item, queue->storage + (queue->head * queue->item_size), queue->item_size);
            queue->head = (queue->head + 1) % queue->capacity;
            queue->count--;
            pthread_mutex_unlock(&queue->lock);
            return pdTRUE;
        }
        pthread_mutex_unlock(&queue->lock);

        if (ticks_to_wait == 0) {
            return pdFALSE;
        }
        usleep(1000);
    }
}

static void *TaskTrampoline(void *arg)
{
    StaticTask_t *task = (StaticTask_t *)arg;
    task->func(task->arg);
    return NULL;
}

TaskHandle_t xTaskCreateStatic(TaskFunction_t task_func, const char *name, uint32_t stack_depth,
                                void *params, UBaseType_t priority, StackType_t *stack_buffer,
                                StaticTask_t *task_buffer)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)stack_buffer;

    task_buffer->func = task_func;
    task_buffer->arg = params;
    if (pthread_create(&task_buffer->thread, NULL, TaskTrampoline, task_buffer) != 0) {
        return NULL;
    }
    pthread_detach(task_buffer->thread);
    return task_buffer;
}
