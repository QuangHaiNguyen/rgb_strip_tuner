#pragma once
/** @file gpio_fakes.h @brief FFF fakes for the GPIO driver used by button.c. */
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Reset fakes; the pin reads high (released). */
void TestGpioReset(void);
/** Level returned by gpio_get_level(): 0 = pressed, 1 = released. */
void TestGpioSetLevel(int level);
/** Make gpio_config() fail. */
void TestGpioFailConfig(bool fails);
/** Copy of the last gpio_config() argument. */
const gpio_config_t *TestGpioLastConfig(void);
int TestGpioConfigCalls(void);
/** Number of gpio_get_level() calls and the fake time (ms) of call @p index. */
int TestGpioReadCount(void);
uint32_t TestGpioReadTimeMs(int index);
int TestGpioLastReadPin(void);
#ifdef __cplusplus
}
#endif
