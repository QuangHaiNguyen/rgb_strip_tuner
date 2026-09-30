/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file led_controller.c
 * @brief WS2812 RMT TX driver: fixed 6-LED pattern, tunable timing (SPEC-004).
 *
 * Owns GPIO8 and one RMT TX channel. A dedicated driver task performs every
 * transmission asynchronously: the ESP-IDF RMT driver's interrupt-driven
 * symbol refill and completion primitive (rmt_tx_wait_all_done()) are used
 * instead of a busy-wait or GPIO poll (FR-13/NFR-1). Callers only ever
 * overwrite a 1-deep queue (FR-7); the actual transmission runs exclusively
 * in the driver task.
 *
 * The WS2812 frame (144 GRB data-bit symbols + 1 reset symbol) is produced
 * by a small custom RMT encoder chaining the driver's bytes encoder (data)
 * and copy encoder (reset), statically allocated (no malloc/free) rather
 * than through rmt_alloc_encoder_mem(), matching this project's "no
 * malloc/free in project code" rule.
 */
#include "led_controller.h"
#include "logging.h"
#include "rmt_pulse_monitor.h"
#include <string.h>
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/**
 * @brief RMT channel memory for the TX channel, in symbols (section 5, NFR-16).
 *
 * Exactly one ESP32-C3 hardware block (SOC_RMT_MEM_WORDS_PER_CHANNEL = 48). A larger value
 * is rounded up to two blocks by the driver (rmt_tx.c, rmt_tx_register_to_group()), which
 * would also occupy the neighbouring TX channel's block. The driver's ISR refills the frame
 * in 24-symbol ping-pong halves, so the 145-symbol frame need not fit in hardware memory.
 */
#define LED_CONTROLLER_MEM_BLOCK_SYMBOLS (48)
#define LED_CONTROLLER_TASK_STACK_BYTES (3072)
#define LED_CONTROLLER_TASK_PRIORITY (4)
/** @brief Bound on StartLedController() waiting for the boot-time frame (FR-1d); a coding-stage choice (section 8.1). */
#define LED_CONTROLLER_BOOT_SYNC_TIMEOUT_MS (50)

LOG_MODULE_REGISTER("led_ctrl", LOG_LEVEL_DEBUG);

/** @brief Custom RMT encoder: WS2812 GRB bytes, then one trailing reset symbol (FR-10/FR-11). */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes_encoder;
    rmt_encoder_handle_t copy_encoder;
    int state;
    rmt_symbol_word_t reset_symbol;
} ws2812_encoder_t;

static rmt_channel_handle_t s_tx_channel;
static ws2812_encoder_t s_encoder;
static uint8_t s_pixel_grb[LED_CONTROLLER_PIXEL_BYTES];

static StaticTask_t s_task_struct;
static StackType_t s_task_stack[LED_CONTROLLER_TASK_STACK_BYTES];
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[sizeof(ws2812_timing_t)];
static QueueHandle_t s_timing_queue;

static StaticSemaphore_t s_boot_done_sem_struct;
static SemaphoreHandle_t s_boot_done_sem;

/* Driver-task-only state: true while a frame whose completion wait timed out may still be encoding. */
static bool s_is_frame_in_flight;

uint16_t Ws2812NsToTicks(uint32_t duration_ns, uint32_t tick_ns)
{
    return (uint16_t)((duration_ns + tick_ns / 2) / tick_ns);
}

void BuildLedPixelPattern(uint8_t pixel_grb[LED_CONTROLLER_PIXEL_BYTES])
{
    /* GRB, MSB-first wire order (section 7.1): LEDs 0-1 red, 2-3 green, 4-5 blue. */
    static const uint8_t pattern[LED_CONTROLLER_PIXEL_BYTES] = {
        0x00, LED_CONTROLLER_CHANNEL_VALUE, 0x00, /* LED 0: red */
        0x00, LED_CONTROLLER_CHANNEL_VALUE, 0x00, /* LED 1: red */
        LED_CONTROLLER_CHANNEL_VALUE, 0x00, 0x00, /* LED 2: green */
        LED_CONTROLLER_CHANNEL_VALUE, 0x00, 0x00, /* LED 3: green */
        0x00, 0x00, LED_CONTROLLER_CHANNEL_VALUE, /* LED 4: blue */
        0x00, 0x00, LED_CONTROLLER_CHANNEL_VALUE, /* LED 5: blue */
    };
    memcpy(pixel_grb, pattern, LED_CONTROLLER_PIXEL_BYTES);
}

/** @brief Build the FR-10 bytes-encoder bit0/bit1 symbols from a timing set (pure computation). */
static rmt_bytes_encoder_config_t BuildBytesEncoderConfig(const ws2812_timing_t *timing)
{
    rmt_bytes_encoder_config_t config = {
        .bit0 =
            {
                .level0 = 1,
                .duration0 = Ws2812NsToTicks(timing->bit0_high_ns, LED_CONTROLLER_TICK_NS),
                .level1 = 0,
                .duration1 = Ws2812NsToTicks((uint32_t)(timing->bit0_period_ns - timing->bit0_high_ns),
                                              LED_CONTROLLER_TICK_NS),
            },
        .bit1 =
            {
                .level0 = 1,
                .duration0 = Ws2812NsToTicks(timing->bit1_high_ns, LED_CONTROLLER_TICK_NS),
                .level1 = 0,
                .duration1 = Ws2812NsToTicks((uint32_t)(timing->bit1_period_ns - timing->bit1_high_ns),
                                              LED_CONTROLLER_TICK_NS),
            },
    };
    config.flags.msb_first = 1;
    return config;
}

/** @brief Build the FR-11 trailing reset symbol from a timing set (pure computation). */
static rmt_symbol_word_t BuildResetSymbol(const ws2812_timing_t *timing)
{
    rmt_symbol_word_t reset = {
        .level0 = 0,
        .duration0 = Ws2812NsToTicks((uint32_t)timing->reset_us * 1000u, LED_CONTROLLER_TICK_NS),
        .level1 = 0,
        .duration1 = 0,
    };
    return reset;
}

/** @brief Encode callback: 144 data-bit symbols via the bytes encoder, then the reset symbol via the copy encoder. */
static size_t EncodeWs2812Frame(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data,
                                 size_t data_size, rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *self = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    if (self->state == 0) {
        encoded_symbols += self->bytes_encoder->encode(self->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            self->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = RMT_ENCODING_MEM_FULL;
            return encoded_symbols;
        }
    }
    if (self->state == 1) {
        encoded_symbols += self->copy_encoder->encode(self->copy_encoder, channel, &self->reset_symbol,
                                                       sizeof(self->reset_symbol), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            self->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ResetWs2812Encoder(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *self = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(self->bytes_encoder);
    rmt_encoder_reset(self->copy_encoder);
    self->state = 0;
    return ESP_OK;
}

static esp_err_t DeleteWs2812Encoder(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *self = __containerof(encoder, ws2812_encoder_t, base);
    if (self->bytes_encoder != NULL) {
        rmt_del_encoder(self->bytes_encoder);
        self->bytes_encoder = NULL;
    }
    if (self->copy_encoder != NULL) {
        rmt_del_encoder(self->copy_encoder);
        self->copy_encoder = NULL;
    }
    return ESP_OK;
}

/** @brief Create the two-stage GRB + reset encoder, statically allocated (no malloc/free). */
static bool InitWs2812Encoder(const ws2812_timing_t *timing)
{
    s_encoder.base.encode = EncodeWs2812Frame;
    s_encoder.base.reset = ResetWs2812Encoder;
    s_encoder.base.del = DeleteWs2812Encoder;
    s_encoder.state = 0;

    rmt_bytes_encoder_config_t bytes_config = BuildBytesEncoderConfig(timing);
    if (rmt_new_bytes_encoder(&bytes_config, &s_encoder.bytes_encoder) != ESP_OK) {
        LOG_ERROR("failed to create WS2812 bytes encoder");
        return false;
    }
    rmt_copy_encoder_config_t copy_config = {};
    if (rmt_new_copy_encoder(&copy_config, &s_encoder.copy_encoder) != ESP_OK) {
        LOG_ERROR("failed to create WS2812 reset encoder");
        return false;
    }
    s_encoder.reset_symbol = BuildResetSymbol(timing);
    return true;
}

/** @brief Update the encoder for a new timing set (FR-10/FR-11); reused across frames. */
static bool UpdateWs2812Encoder(const ws2812_timing_t *timing)
{
    rmt_bytes_encoder_config_t bytes_config = BuildBytesEncoderConfig(timing);
    if (rmt_bytes_encoder_update_config(s_encoder.bytes_encoder, &bytes_config) != ESP_OK) {
        LOG_ERROR("led_controller: failed to update bit encoding");
        return false;
    }
    s_encoder.reset_symbol = BuildResetSymbol(timing);
    return true;
}

/**
 * @brief Check that the frame whose completion wait timed out has finished (FR-16).
 *
 * The encoder must not be updated while a previous frame may still be encoding. If the
 * previous wait timed out, wait once more with the same bounded, >= 2-tick timeout; if the
 * frame is still in flight, the new frame is skipped with a Warning (the next queued update
 * checks again). No channel restart.
 *
 * @return true if no earlier frame is still in flight.
 */
static bool WaitForPreviousFrame(void)
{
    if (!s_is_frame_in_flight) {
        return true;
    }
    if (rmt_tx_wait_all_done(s_tx_channel, LED_CONTROLLER_FRAME_TIMEOUT_MS) != ESP_OK) {
        LOG_WARNING("led_controller: previous frame still in flight, frame skipped");
        return false;
    }
    s_is_frame_in_flight = false;
    return true;
}

/**
 * @brief Build one frame, arm the pulse monitor, transmit and await completion (FR-13, FR-16, FR-18).
 *
 * rmt_transmit() is non-blocking (flags.queue_nonblocking): with the previous frame still in
 * flight it returns ESP_ERR_INVALID_STATE at once, which is logged as a transient Warning.
 * Any other transmit error and an encoder update failure are channel/encoder-level Errors.
 */
static void DriveFrame(const ws2812_timing_t *timing)
{
    if (!WaitForPreviousFrame()) {
        return;
    }
    if (!UpdateWs2812Encoder(timing)) {
        return;
    }

    /* Arm the pulse monitor immediately before rmt_transmit(), per its documented sequencing (FR-24). */
    ArmPulseCapture(timing, s_pixel_grb, LED_CONTROLLER_PIXEL_BYTES);

    rmt_transmit_config_t transmit_config = {0};
    transmit_config.flags.queue_nonblocking = 1;
    esp_err_t result = rmt_transmit(s_tx_channel, &s_encoder.base, s_pixel_grb, LED_CONTROLLER_PIXEL_BYTES, &transmit_config);
    if (result == ESP_ERR_INVALID_STATE) {
        LOG_WARNING("led_controller: transmit busy, previous frame in flight (err=%d)", (int)result);
        return;
    }
    if (result != ESP_OK) {
        LOG_ERROR("led_controller: transmit failed (err=%d)", (int)result);
        return;
    }
    result = rmt_tx_wait_all_done(s_tx_channel, LED_CONTROLLER_FRAME_TIMEOUT_MS);
    if (result != ESP_OK) {
        s_is_frame_in_flight = true;
        LOG_WARNING("led_controller: frame completion timed out (err=%d)", (int)result);
        return;
    }

    LOG_INFO("led_controller: strip driven");
    LOG_DEBUG("led_controller: bit0 high_ns=%u period_ns=%u; bit1 high_ns=%u period_ns=%u; reset_us=%u",
              timing->bit0_high_ns, timing->bit0_period_ns, timing->bit1_high_ns, timing->bit1_period_ns,
              timing->reset_us);
}

static void RunLedDriverTask(void *arg)
{
    (void)arg;
    bool is_boot_frame = true;
    for (;;) {
        ws2812_timing_t timing;
        xQueueReceive(s_timing_queue, &timing, portMAX_DELAY);
        DriveFrame(&timing);
        if (is_boot_frame) {
            is_boot_frame = false;
            xSemaphoreGive(s_boot_done_sem);
        }
    }
}

void ApplyWs2812Timing(const ws2812_timing_t *timing)
{
    if (timing == NULL || s_timing_queue == NULL) {
        return;
    }
    xQueueOverwrite(s_timing_queue, timing);
}

bool StartLedController(void)
{
    const ws2812_timing_t default_timing = {
        .bit0_high_ns = TUNER_DEFAULT_BIT0_HIGH_NS,
        .bit0_period_ns = TUNER_DEFAULT_BIT0_PERIOD_NS,
        .bit1_high_ns = TUNER_DEFAULT_BIT1_HIGH_NS,
        .bit1_period_ns = TUNER_DEFAULT_BIT1_PERIOD_NS,
        .reset_us = TUNER_DEFAULT_RESET_US,
    };

    BuildLedPixelPattern(s_pixel_grb);

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = LED_CONTROLLER_GPIO_NUM,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_CONTROLLER_RESOLUTION_HZ,
        .mem_block_symbols = LED_CONTROLLER_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 1,
    };
    if (rmt_new_tx_channel(&tx_config, &s_tx_channel) != ESP_OK) {
        LOG_ERROR("failed to create RMT TX channel on GPIO %d", LED_CONTROLLER_GPIO_NUM);
        s_tx_channel = NULL;
        return false;
    }

    if (!InitWs2812Encoder(&default_timing)) {
        s_tx_channel = NULL;
        return false;
    }

    if (rmt_enable(s_tx_channel) != ESP_OK) {
        LOG_ERROR("failed to enable RMT TX channel");
        s_tx_channel = NULL;
        return false;
    }

    s_timing_queue = xQueueCreateStatic(1, sizeof(ws2812_timing_t), s_queue_storage, &s_queue_struct);
    s_boot_done_sem = xSemaphoreCreateBinaryStatic(&s_boot_done_sem_struct);

    TaskHandle_t handle = xTaskCreateStatic(RunLedDriverTask, "led_ctrl", LED_CONTROLLER_TASK_STACK_BYTES, NULL,
                                             LED_CONTROLLER_TASK_PRIORITY, s_task_stack, &s_task_struct);
    if (handle == NULL) {
        LOG_ERROR("failed to create led_controller driver task");
        s_tx_channel = NULL;
        return false;
    }

    /* FR-1d: drive one boot-time frame before returning. The actual transmission still runs
     * exclusively in the driver task (FR-7); StartLedController() only waits for its completion. */
    ApplyWs2812Timing(&default_timing);
    if (xSemaphoreTake(s_boot_done_sem, pdMS_TO_TICKS(LED_CONTROLLER_BOOT_SYNC_TIMEOUT_MS)) != pdTRUE) {
        LOG_WARNING("led_controller: boot frame not confirmed within %d ms", LED_CONTROLLER_BOOT_SYNC_TIMEOUT_MS);
    }

    LOG_INFO("led_controller started on GPIO %d", LED_CONTROLLER_GPIO_NUM);
    return true;
}
