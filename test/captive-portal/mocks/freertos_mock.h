#pragma once
/**
 * @file freertos_mock.h
 * @brief Deterministic FreeRTOS simulation for host tests.
 *
 * Tasks created with xTaskCreateStatic() run as coroutines. They only make
 * progress inside MockRunTask(), and every blocking call (queue receive with a
 * timeout, vTaskDelay, vTaskDelayUntil) advances a fake millisecond clock
 * instead of sleeping, so hours of firmware time run instantly and exactly.
 */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Forget all tasks, reset the clock to 0 and the mutex counters. */
void MockFreeRtosReset(void);
/** Current fake time in milliseconds. */
uint32_t MockGetNowMs(void);
/** Number of tasks created since the last reset. */
int MockGetTaskCount(void);
/** Name given to task @p index. */
const char *MockGetTaskName(int index);
/** Run task @p index until fake time reaches @p until_ms (or the task ends). */
void MockRunTask(int index, uint32_t until_ms);
/** Pretend task @p index has been deleted. */
void MockMarkTaskDeleted(int index);
/** Mutex takes minus gives; 0 means every take was released. */
int MockGetMutexBalance(void);

#ifdef __cplusplus
}
#endif
