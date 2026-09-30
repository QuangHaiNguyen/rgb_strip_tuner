/* FFF fakes of the ESP-IDF RMT driver; see rmt_fakes.h. FFF globals live in log_fakes.c. */
#include "rmt_fakes.h"
#include <string.h>

DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_tx_channel, const rmt_tx_channel_config_t *, rmt_channel_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_rx_channel, const rmt_rx_channel_config_t *, rmt_channel_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_enable, rmt_channel_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_disable, rmt_channel_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_bytes_encoder, const rmt_bytes_encoder_config_t *, rmt_encoder_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_bytes_encoder_update_config, rmt_encoder_handle_t,
                       const rmt_bytes_encoder_config_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_new_copy_encoder, const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_del_encoder, rmt_encoder_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_encoder_reset, rmt_encoder_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_transmit, rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                       const rmt_transmit_config_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_tx_wait_all_done, rmt_channel_handle_t, int);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_rx_register_event_callbacks, rmt_channel_handle_t,
                       const rmt_rx_event_callbacks_t *, void *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, rmt_receive, rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *);

static char s_tx_channel_token;
static char s_rx_channel_token;
const rmt_channel_handle_t kFakeTxChannel = (rmt_channel_handle_t)&s_tx_channel_token;
const rmt_channel_handle_t kFakeRxChannel = (rmt_channel_handle_t)&s_rx_channel_token;
rmt_encoder_t g_fake_bytes_encoder;
rmt_encoder_t g_fake_copy_encoder;

rmt_tx_channel_config_t g_last_tx_config;
rmt_rx_channel_config_t g_last_rx_config;
rmt_bytes_encoder_config_t g_last_bytes_config;
rmt_bytes_encoder_config_t g_last_bytes_update_config;
rmt_rx_event_callbacks_t g_last_rx_callbacks;
rmt_receive_config_t g_last_receive_config;

static esp_err_t NewTxChannelFake(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *ret_chan)
{
    g_last_tx_config = *config;
    if (rmt_new_tx_channel_fake.return_val == ESP_OK) {
        *ret_chan = kFakeTxChannel;
    }
    return rmt_new_tx_channel_fake.return_val;
}

static esp_err_t NewRxChannelFake(const rmt_rx_channel_config_t *config, rmt_channel_handle_t *ret_chan)
{
    g_last_rx_config = *config;
    if (rmt_new_rx_channel_fake.return_val == ESP_OK) {
        *ret_chan = kFakeRxChannel;
    }
    return rmt_new_rx_channel_fake.return_val;
}

static esp_err_t NewBytesEncoderFake(const rmt_bytes_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    g_last_bytes_config = *config;
    if (rmt_new_bytes_encoder_fake.return_val == ESP_OK) {
        *ret_encoder = &g_fake_bytes_encoder;
    }
    return rmt_new_bytes_encoder_fake.return_val;
}

static esp_err_t UpdateBytesEncoderFake(rmt_encoder_handle_t encoder, const rmt_bytes_encoder_config_t *config)
{
    (void)encoder;
    g_last_bytes_update_config = *config;
    return rmt_bytes_encoder_update_config_fake.return_val;
}

static esp_err_t NewCopyEncoderFake(const rmt_copy_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    (void)config;
    if (rmt_new_copy_encoder_fake.return_val == ESP_OK) {
        *ret_encoder = &g_fake_copy_encoder;
    }
    return rmt_new_copy_encoder_fake.return_val;
}

static esp_err_t RegisterRxCallbacksFake(rmt_channel_handle_t channel, const rmt_rx_event_callbacks_t *cbs, void *user)
{
    (void)channel;
    (void)user;
    g_last_rx_callbacks = *cbs;
    return rmt_rx_register_event_callbacks_fake.return_val;
}

static esp_err_t ReceiveFake(rmt_channel_handle_t channel, void *buffer, size_t size, const rmt_receive_config_t *config)
{
    (void)channel;
    (void)buffer;
    (void)size;
    g_last_receive_config = *config;
    return rmt_receive_fake.return_val;
}

void RmtFakesReset(void)
{
    RESET_FAKE(rmt_new_tx_channel);
    RESET_FAKE(rmt_new_rx_channel);
    RESET_FAKE(rmt_enable);
    RESET_FAKE(rmt_disable);
    RESET_FAKE(rmt_new_bytes_encoder);
    RESET_FAKE(rmt_bytes_encoder_update_config);
    RESET_FAKE(rmt_new_copy_encoder);
    RESET_FAKE(rmt_del_encoder);
    RESET_FAKE(rmt_encoder_reset);
    RESET_FAKE(rmt_transmit);
    RESET_FAKE(rmt_tx_wait_all_done);
    RESET_FAKE(rmt_rx_register_event_callbacks);
    RESET_FAKE(rmt_receive);

    /* return_val (ESP_OK after reset) stays the knob tests turn; the custom fakes honor it. */
    rmt_new_tx_channel_fake.custom_fake = NewTxChannelFake;
    rmt_new_rx_channel_fake.custom_fake = NewRxChannelFake;
    rmt_new_bytes_encoder_fake.custom_fake = NewBytesEncoderFake;
    rmt_bytes_encoder_update_config_fake.custom_fake = UpdateBytesEncoderFake;
    rmt_new_copy_encoder_fake.custom_fake = NewCopyEncoderFake;
    rmt_rx_register_event_callbacks_fake.custom_fake = RegisterRxCallbacksFake;
    rmt_receive_fake.custom_fake = ReceiveFake;

    memset(&g_fake_bytes_encoder, 0, sizeof(g_fake_bytes_encoder));
    memset(&g_fake_copy_encoder, 0, sizeof(g_fake_copy_encoder));
    memset(&g_last_tx_config, 0, sizeof(g_last_tx_config));
    memset(&g_last_rx_config, 0, sizeof(g_last_rx_config));
    memset(&g_last_bytes_config, 0, sizeof(g_last_bytes_config));
    memset(&g_last_bytes_update_config, 0, sizeof(g_last_bytes_update_config));
    memset(&g_last_rx_callbacks, 0, sizeof(g_last_rx_callbacks));
    memset(&g_last_receive_config, 0, sizeof(g_last_receive_config));
}
