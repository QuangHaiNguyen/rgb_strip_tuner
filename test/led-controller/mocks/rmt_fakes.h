#pragma once
/**
 * @file rmt_fakes.h
 * @brief FFF fakes of the ESP-IDF RMT TX/RX driver API (hardware; never exercised for real on the host).
 *
 * RmtFakesReset() makes every call succeed (ESP_OK). The channel constructors return the fixed
 * handles below; the encoder constructors return the two stub encoders below, whose encode/reset
 * callbacks tests can replace. Constructor configs are copied (the code passes stack addresses).
 */
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "fff.h"

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_tx_channel, const rmt_tx_channel_config_t *, rmt_channel_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_rx_channel, const rmt_rx_channel_config_t *, rmt_channel_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_enable, rmt_channel_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_disable, rmt_channel_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_bytes_encoder, const rmt_bytes_encoder_config_t *, rmt_encoder_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_bytes_encoder_update_config, rmt_encoder_handle_t,
                        const rmt_bytes_encoder_config_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_copy_encoder, const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_del_encoder, rmt_encoder_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_encoder_reset, rmt_encoder_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_transmit, rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                        const rmt_transmit_config_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_tx_wait_all_done, rmt_channel_handle_t, int);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_rx_register_event_callbacks, rmt_channel_handle_t,
                        const rmt_rx_event_callbacks_t *, void *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, rmt_receive, rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *);

/** Handles returned by rmt_new_tx_channel() / rmt_new_rx_channel(). */
extern const rmt_channel_handle_t kFakeTxChannel;
extern const rmt_channel_handle_t kFakeRxChannel;
/** Stub encoders returned by rmt_new_bytes_encoder() / rmt_new_copy_encoder(). */
extern rmt_encoder_t g_fake_bytes_encoder;
extern rmt_encoder_t g_fake_copy_encoder;

/** Copies of the last config passed to each call below (valid after at least one call). */
extern rmt_tx_channel_config_t g_last_tx_config;
extern rmt_rx_channel_config_t g_last_rx_config;
extern rmt_bytes_encoder_config_t g_last_bytes_config;          /* rmt_new_bytes_encoder() */
extern rmt_bytes_encoder_config_t g_last_bytes_update_config;   /* rmt_bytes_encoder_update_config() */
extern rmt_rx_event_callbacks_t g_last_rx_callbacks;
extern rmt_receive_config_t g_last_receive_config;

/** Reset every RMT fake and the captured configs; install the default (all succeed) behavior. */
void RmtFakesReset(void);

#ifdef __cplusplus
}
#endif
