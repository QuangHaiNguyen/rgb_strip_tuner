#pragma once
/**
 * @file rmt_encoder.h
 * @brief Host replacement for ESP-IDF driver/rmt_encoder.h; functions are FFF fakes (rmt_fakes.c).
 */
#include <stddef.h>
#include "driver/rmt_types.h"

#ifndef __containerof
/* Provided by the ESP-IDF toolchain's sys/cdefs.h on target. */
#define __containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RMT_ENCODING_RESET = 0,
    RMT_ENCODING_COMPLETE = (1 << 0),
    RMT_ENCODING_MEM_FULL = (1 << 1),
    RMT_ENCODING_WITH_EOF = (1 << 2),
} rmt_encode_state_t;

typedef struct rmt_encoder_t rmt_encoder_t;
struct rmt_encoder_t {
    size_t (*encode)(rmt_encoder_t *encoder, rmt_channel_handle_t tx_channel, const void *primary_data,
                     size_t data_size, rmt_encode_state_t *ret_state);
    esp_err_t (*reset)(rmt_encoder_t *encoder);
    esp_err_t (*del)(rmt_encoder_t *encoder);
};

typedef struct {
    rmt_symbol_word_t bit0;
    rmt_symbol_word_t bit1;
    struct {
        uint32_t msb_first : 1;
    } flags;
} rmt_bytes_encoder_config_t;

typedef struct {
    int unused; /* empty in ESP-IDF; one member here so the type has the same size in C and C++ */
} rmt_copy_encoder_config_t;

esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);
esp_err_t rmt_bytes_encoder_update_config(rmt_encoder_handle_t bytes_encoder, const rmt_bytes_encoder_config_t *config);
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);
esp_err_t rmt_encoder_reset(rmt_encoder_handle_t encoder);

#ifdef __cplusplus
}
#endif
