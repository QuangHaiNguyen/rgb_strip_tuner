#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;

#define pdTRUE  1
#define pdFALSE 0

#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFUL)
#define portTICK_PERIOD_MS  (1U)
#define tskIDLE_PRIORITY    (0U)
