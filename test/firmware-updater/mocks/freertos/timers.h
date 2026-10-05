#pragma once
/* Host mock of freertos/timers.h: the static one-shot timer of the updater's delayed restart (FR-28). */
#include "freertos/FreeRTOS.h"
#ifndef pdPASS
#define pdPASS (pdTRUE)
#endif
typedef struct { int unused; } StaticTimer_t;
typedef void *TimerHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t timer);
#ifdef __cplusplus
extern "C" {
#endif
TimerHandle_t xTimerCreateStatic(const char *name, TickType_t period_ticks, UBaseType_t auto_reload, void *timer_id,
                                 TimerCallbackFunction_t callback, StaticTimer_t *timer_buffer);
BaseType_t xTimerStart(TimerHandle_t timer, TickType_t ticks_to_wait);
#ifdef __cplusplus
}
#endif
