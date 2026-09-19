#pragma once

#include <stdint.h>

#include "fff.h"

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_FAKE_VALUE_FUNC(int64_t, esp_timer_get_time);

#ifdef __cplusplus
}
#endif
