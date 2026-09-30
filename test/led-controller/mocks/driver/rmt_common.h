#pragma once
/**
 * @file rmt_common.h
 * @brief Host replacement for ESP-IDF driver/rmt_common.h; functions are FFF fakes (rmt_fakes.c).
 */
#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
#ifdef __cplusplus
}
#endif
