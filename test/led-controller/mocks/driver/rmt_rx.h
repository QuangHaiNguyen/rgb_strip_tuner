#pragma once
/**
 * @file rmt_rx.h
 * @brief Host replacement for ESP-IDF driver/rmt_rx.h; functions are FFF fakes (rmt_fakes.c).
 */
#include "driver/rmt_common.h"
#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rmt_rx_done_callback_t on_recv_done;
} rmt_rx_event_callbacks_t;

typedef struct {
    gpio_num_t gpio_num;
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    int intr_priority;
    struct {
        uint32_t invert_in : 1;
        uint32_t with_dma : 1;
        uint32_t allow_pd : 1;
    } flags;
} rmt_rx_channel_config_t;

typedef struct {
    uint32_t signal_range_min_ns;
    uint32_t signal_range_max_ns;
    struct extra_rmt_receive_flags {
        uint32_t en_partial_rx : 1;
    } flags;
} rmt_receive_config_t;

esp_err_t rmt_new_rx_channel(const rmt_rx_channel_config_t *config, rmt_channel_handle_t *ret_chan);
esp_err_t rmt_rx_register_event_callbacks(rmt_channel_handle_t rx_channel, const rmt_rx_event_callbacks_t *cbs,
                                          void *user_data);
esp_err_t rmt_receive(rmt_channel_handle_t rx_channel, void *buffer, size_t buffer_size,
                      const rmt_receive_config_t *config);

#ifdef __cplusplus
}
#endif
