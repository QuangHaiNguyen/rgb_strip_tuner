#pragma once
/**
 * @file rmt_types.h
 * @brief Host replacement for ESP-IDF driver/rmt_types.h (+ hal/rmt_types.h), IDF v6.0-beta2 layout.
 *
 * Only the types the SPEC-004 components use. rmt_symbol_word_t keeps the hardware bit layout
 * (15-bit durations) so a value that would not fit the hardware field is truncated here too.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

typedef enum {
    RMT_CLK_SRC_DEFAULT = 0,
} rmt_clock_source_t;

/** @brief The layout of one RMT symbol in memory, decided by the hardware (hal/rmt_types.h). */
typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;

typedef struct rmt_channel_t *rmt_channel_handle_t;
typedef struct rmt_encoder_t *rmt_encoder_handle_t;

typedef struct {
    rmt_symbol_word_t *received_symbols;
    size_t num_symbols;
    struct {
        uint32_t is_last : 1;
    } flags;
} rmt_rx_done_event_data_t;

typedef bool (*rmt_rx_done_callback_t)(rmt_channel_handle_t rx_chan, const rmt_rx_done_event_data_t *edata,
                                       void *user_ctx);

#ifdef __cplusplus
}
#endif
