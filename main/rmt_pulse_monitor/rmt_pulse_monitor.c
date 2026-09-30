/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file rmt_pulse_monitor.c
 * @brief RMT RX pulse measurement of the WS2812 waveform led_controller transmits (SPEC-004 section 3.6).
 *
 * Owns GPIO4 and one RMT RX channel (distinct hardware from led_controller's
 * TX channel, NFR-16). ArmPulseCapture() is called by led_controller
 * immediately before every rmt_transmit() call; the RX channel captures the
 * same waveform, and a dedicated decode task classifies and logs it. This
 * component never gates or blocks a WS2812 frame transmission (section 3.6).
 *
 * Arm/capture pairing (FR-24, FR-25, FR-31): every arm attempt gets a sequence
 * number. The on_recv_done ISR tags its capture-done event with the sequence
 * number of the receive it completes. The decode task decodes an event only if
 * its tag equals the last successfully armed sequence number, checked under
 * s_rx_lock; while holding the lock it copies the capture buffer and the armed
 * snapshot, so a newer arm cannot overwrite them mid-decode. Any other event is
 * stale and discarded without ending the wait. The decode task also records the
 * sequence number it is waiting on; after a timeout, the RX restart is skipped
 * only if a newer sequence number has been armed since, so each timed-out arm
 * gets exactly one Warning and an immediate restart.
 */
#include "rmt_pulse_monitor.h"
#include "logging.h"
#include <string.h>
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/**
 * @brief RMT channel memory for the RX channel, in symbols (section 5.1, NFR-16).
 *
 * Exactly one ESP32-C3 hardware block (SOC_RMT_MEM_WORDS_PER_CHANNEL = 48). A larger value
 * is rounded up to two blocks by the driver (rmt_rx.c, rmt_rx_register_to_group()). The RX
 * ping-pong ISR copies each 24-symbol half into the capture buffer as it fills, so all 144
 * symbols stream through one block.
 */
#define RMT_PULSE_MONITOR_MEM_BLOCK_SYMBOLS (48)
#define RMT_PULSE_MONITOR_TASK_STACK_BYTES (2048)
#define RMT_PULSE_MONITOR_TASK_PRIORITY (2)

LOG_MODULE_REGISTER("pulse_mon", LOG_LEVEL_DEBUG);

/** @brief One capture-done event posted by the ISR callback (FR-25). */
typedef struct {
    size_t symbol_count;  /**< Number of RMT symbols received. */
    uint32_t arm_seq;     /**< Sequence number of the arm whose receive completed. */
} capture_done_event_t;

static rmt_channel_handle_t s_rx_channel;

/* Static capture buffer (FR-23): 576 bytes, filled directly by the RX driver. */
static rmt_symbol_word_t s_symbol_buffer[RMT_PULSE_MONITOR_SYMBOL_CAPACITY];

/* Snapshot written by ArmPulseCapture() (FR-24) under s_rx_lock; copied out by the decode task under s_rx_lock. */
static ws2812_timing_t s_armed_timing;
static uint8_t s_armed_pixel_grb[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES];
static size_t s_armed_pixel_len;
/* Sequence number of the last successful arm; read and written only under s_rx_lock. */
static uint32_t s_armed_seq;
/* Sequence number the ISR tags its done event with. Written only under s_rx_lock; read by the
 * ISR, which cannot take a mutex (a single aligned 32-bit read is atomic on the ESP32-C3). */
static volatile uint32_t s_capture_seq;

/* Decode task's private copies of one matched capture, taken under s_rx_lock (no sharing afterwards). */
static rmt_symbol_word_t s_decode_symbols[RMT_PULSE_MONITOR_SYMBOL_CAPACITY];
static ws2812_timing_t s_decode_timing;
static uint8_t s_decode_pixel_grb[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES];
static size_t s_decode_pixel_len;

static StaticTask_t s_task_struct;
static StackType_t s_task_stack[RMT_PULSE_MONITOR_TASK_STACK_BYTES];
static StaticQueue_t s_capture_queue_struct;
static uint8_t s_capture_queue_storage[sizeof(capture_done_event_t)];
static QueueHandle_t s_capture_queue;
/* Given by ArmPulseCapture() after a successful rmt_receive(); starts the FR-31 timeout. */
static StaticSemaphore_t s_armed_sem_struct;
static SemaphoreHandle_t s_armed_sem;
/* Serializes RX channel state changes (rmt_receive() in ArmPulseCapture() vs. the FR-31
 * rmt_disable()/rmt_enable() restart) and guards the armed snapshot, the sequence numbers and
 * the capture buffer. Arming takes it with zero timeout; the decode task holds it only for
 * bounded copies and the restart pair. */
static StaticSemaphore_t s_rx_lock_struct;
static SemaphoreHandle_t s_rx_lock;

uint32_t Ws2812TicksToNs(uint32_t ticks, uint32_t tick_ns)
{
    return ticks * tick_ns;
}

ws2812_symbol_class_t DecodeWs2812Symbol(rmt_symbol_word_t symbol, uint32_t bit0_high_ns, uint32_t bit1_high_ns,
                                          uint32_t tick_ns)
{
    if (symbol.level0 != 1) {
        return WS2812_SYMBOL_INVALID;
    }
    uint32_t high_ns = Ws2812TicksToNs(symbol.duration0, tick_ns);
    uint32_t bit0_distance_ns = (high_ns > bit0_high_ns) ? high_ns - bit0_high_ns : bit0_high_ns - high_ns;
    uint32_t bit1_distance_ns = (high_ns > bit1_high_ns) ? high_ns - bit1_high_ns : bit1_high_ns - high_ns;
    /* Nearest commanded high time wins; an exactly equidistant pulse is bit 0 (tie rule). */
    return (bit0_distance_ns <= bit1_distance_ns) ? WS2812_SYMBOL_BIT0 : WS2812_SYMBOL_BIT1;
}

bool AggregateWs2812Pulses(const rmt_symbol_word_t *symbols, size_t symbol_count, const ws2812_timing_t *applied_timing,
                           const uint8_t *expected_pixel_grb, size_t expected_pixel_len, uint32_t tick_ns,
                           ws2812_pulse_stats_t *stats)
{
    if (symbols == NULL || applied_timing == NULL || expected_pixel_grb == NULL || stats == NULL ||
        symbol_count != RMT_PULSE_MONITOR_DATA_SYMBOLS) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    /* Equal commanded high times: bits cannot be told apart from timing (FR-28). */
    stats->match_available = (applied_timing->bit0_high_ns != applied_timing->bit1_high_ns);

    bool has_bit0_example = false;
    bool has_bit1_example = false;
    uint64_t bit0_sum_ns = 0;
    uint64_t bit1_sum_ns = 0;
    uint32_t bit0_count = 0;
    uint32_t bit1_count = 0;
    uint32_t bit0_min_ns = UINT32_MAX;
    uint32_t bit1_min_ns = UINT32_MAX;
    uint32_t match_count = 0;

    for (size_t index = 0; index < RMT_PULSE_MONITOR_DATA_SYMBOLS; ++index) {
        rmt_symbol_word_t symbol = symbols[index];
        ws2812_symbol_class_t symbol_class =
            DecodeWs2812Symbol(symbol, applied_timing->bit0_high_ns, applied_timing->bit1_high_ns, tick_ns);

        size_t byte_index = index / 8;
        size_t bit_in_byte = 7 - (index % 8);
        bool expected_bit = (byte_index < expected_pixel_len) &&
                             (((expected_pixel_grb[byte_index] >> bit_in_byte) & 0x01) != 0);

        if (symbol_class == WS2812_SYMBOL_INVALID) {
            continue; /* excluded from timing stats; never matches (FR-28d, FR-32) */
        }

        uint32_t high_ns = Ws2812TicksToNs(symbol.duration0, tick_ns);
        uint32_t low_ns = Ws2812TicksToNs(symbol.duration1, tick_ns);
        /* The final symbol's duration1 is the RMT end marker, not a measured low time. */
        bool has_low_time = (index + 1 < RMT_PULSE_MONITOR_DATA_SYMBOLS);
        bool decoded_bit = (symbol_class == WS2812_SYMBOL_BIT1);
        if (stats->match_available && decoded_bit == expected_bit) {
            ++match_count;
        }
        /* Stats follow the decoded bit, or the expected bit when the timing cannot tell them apart. */
        bool stats_bit = stats->match_available ? decoded_bit : expected_bit;

        if (!stats_bit) {
            if (!has_bit0_example && has_low_time) {
                stats->bit0_first_high_ns = high_ns;
                stats->bit0_first_low_ns = low_ns;
                has_bit0_example = true;
            }
            bit0_sum_ns += high_ns;
            ++bit0_count;
            if (high_ns < bit0_min_ns) {
                bit0_min_ns = high_ns;
            }
            if (high_ns > stats->bit0_high_max_ns) {
                stats->bit0_high_max_ns = high_ns;
            }
        } else {
            if (!has_bit1_example && has_low_time) {
                stats->bit1_first_high_ns = high_ns;
                stats->bit1_first_low_ns = low_ns;
                has_bit1_example = true;
            }
            bit1_sum_ns += high_ns;
            ++bit1_count;
            if (high_ns < bit1_min_ns) {
                bit1_min_ns = high_ns;
            }
            if (high_ns > stats->bit1_high_max_ns) {
                stats->bit1_high_max_ns = high_ns;
            }
        }
    }

    stats->bit0_high_min_ns = (bit0_count > 0) ? bit0_min_ns : 0;
    stats->bit0_high_avg_ns = (bit0_count > 0) ? (uint32_t)((bit0_sum_ns + bit0_count / 2) / bit0_count) : 0;
    stats->bit1_high_min_ns = (bit1_count > 0) ? bit1_min_ns : 0;
    stats->bit1_high_avg_ns = (bit1_count > 0) ? (uint32_t)((bit1_sum_ns + bit1_count / 2) / bit1_count) : 0;
    stats->match_count = match_count;
    return true;
}

/**
 * @brief Minimal ISR-safe hand-off: enqueue the received symbol count, tagged with its arm sequence number (FR-25).
 *
 * Overwrites the 1-deep queue, so an unconsumed older (stale) event can never block the
 * newest one; the decode task tells them apart by the tag.
 */
static bool HandleRxDone(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)channel;
    (void)user_ctx;
    capture_done_event_t event = {.symbol_count = edata->num_symbols, .arm_seq = s_capture_seq};
    BaseType_t high_task_woken = pdFALSE;
    xQueueOverwriteFromISR(s_capture_queue, &event, &high_task_woken);
    return high_task_woken == pdTRUE;
}

/** @brief Log one bad data-bit symbol (FR-32); level0 != 1 means unclassifiable. */
static void LogBadSymbolsIfAny(void)
{
    for (size_t index = 0; index < RMT_PULSE_MONITOR_DATA_SYMBOLS; ++index) {
        ws2812_symbol_class_t symbol_class = DecodeWs2812Symbol(
            s_decode_symbols[index], s_decode_timing.bit0_high_ns, s_decode_timing.bit1_high_ns, RMT_PULSE_MONITOR_TICK_NS);
        if (symbol_class == WS2812_SYMBOL_INVALID) {
            LOG_WARNING("pulse monitor: symbol decode error (reason=bad_symbol index=%u)", (unsigned)index);
        }
    }
}

/** @brief Log the three FR-29/section 7.6 Info lines for one successfully decoded capture. */
static void LogPulseStats(const ws2812_pulse_stats_t *stats)
{
    LOG_INFO("pulse: bit0 first high_ns=%u low_ns=%u; bit1 first high_ns=%u low_ns=%u", (unsigned)stats->bit0_first_high_ns,
             (unsigned)stats->bit0_first_low_ns, (unsigned)stats->bit1_first_high_ns, (unsigned)stats->bit1_first_low_ns);
    LOG_INFO("pulse: bit0 high_ns min=%u max=%u avg=%u; bit1 high_ns min=%u max=%u avg=%u",
             (unsigned)stats->bit0_high_min_ns, (unsigned)stats->bit0_high_max_ns, (unsigned)stats->bit0_high_avg_ns,
             (unsigned)stats->bit1_high_min_ns, (unsigned)stats->bit1_high_max_ns, (unsigned)stats->bit1_high_avg_ns);
    if (stats->match_available) {
        LOG_INFO("pulse: grb match=%u/%u", (unsigned)stats->match_count, (unsigned)RMT_PULSE_MONITOR_DATA_SYMBOLS);
    } else {
        LOG_INFO("pulse: grb match=n/a");
    }
}

/** @brief Consume a pending arm signal, if any. The caller holds s_rx_lock, so no arm can give meanwhile. */
static void ClearPendingArmSignal(void)
{
    if (uxSemaphoreGetCount(s_armed_sem) > 0) {
        xSemaphoreTake(s_armed_sem, 0);
    }
}

/**
 * @brief Read the last successfully armed sequence number under s_rx_lock (FR-31).
 *
 * @return The arm sequence number the decode task's capture wait is for.
 */
static uint32_t GetArmedSeq(void)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    uint32_t armed_seq = s_armed_seq;
    xSemaphoreGive(s_rx_lock);
    return armed_seq;
}

/**
 * @brief Abort a pending rmt_receive() after a capture timeout by disabling and re-enabling the RX channel (FR-31).
 *
 * rmt_disable() is accepted in both the enable and run states and returns the
 * channel to the init state with its RX interrupts masked and cleared;
 * rmt_enable() then makes it armable again. Holds s_rx_lock for the whole
 * pair, so ArmPulseCapture() never sees the intermediate init state. If an arm
 * newer than @p timed_out_seq already succeeded (a late done event freed the
 * channel), the restart is skipped so that fresh capture is not aborted;
 * otherwise any pending arm signal can only belong to @p timed_out_seq and is
 * consumed, so its wait never runs a second time. Sequence numbers only
 * increase, so "different" means "newer". On failure, logs one Warning and
 * returns: no retry, no reboot. A channel left disabled is re-enabled once by
 * the next ArmPulseCapture() (FR-24).
 *
 * @param[in] timed_out_seq Arm sequence number whose capture timed out.
 */
static void RestartRxChannel(uint32_t timed_out_seq)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if (s_armed_seq != timed_out_seq) {
        uint32_t armed_seq = s_armed_seq;
        xSemaphoreGive(s_rx_lock);
        LOG_DEBUG("pulse monitor: rx restart skipped, a newer capture is armed (timed_out_seq=%u armed_seq=%u)",
                  (unsigned)timed_out_seq, (unsigned)armed_seq);
        return;
    }
    ClearPendingArmSignal();
    esp_err_t result = rmt_disable(s_rx_channel);
    if (result == ESP_OK) {
        result = rmt_enable(s_rx_channel);
    }
    xSemaphoreGive(s_rx_lock);

    if (result != ESP_OK) {
        LOG_WARNING("pulse monitor: rx restart failed (err=%d)", (int)result);
        return;
    }
    LOG_DEBUG("pulse monitor: rx channel restarted after timeout");
}

/**
 * @brief Take the capture that belongs to the latest successful arm, if @p event is for it (FR-31).
 *
 * Under s_rx_lock (so no newer arm can start a receive into the buffer meanwhile), compares the
 * event's tag with the last armed sequence number and, on a match, copies the capture buffer
 * and the armed snapshot into the decode task's private storage. A match also clears any
 * pending arm signal: it can only belong to this, the latest, arm. A stale event moves the
 * wait on to the latest arm; that arm's pending signal, if any, is consumed by its match or
 * by the timeout restart (RestartRxChannel()).
 *
 * @param[in] event Capture-done event received from the ISR.
 * @param[in,out] wait_seq Arm sequence number the decode task is waiting on; updated on a stale event.
 * @return true if the event is current and its data was copied; false if it is stale.
 */
static bool TakeCurrentCapture(const capture_done_event_t *event, uint32_t *wait_seq)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool is_current = (event->arm_seq == s_armed_seq);
    if (is_current) {
        memcpy(s_decode_symbols, s_symbol_buffer, sizeof(s_decode_symbols));
        s_decode_timing = s_armed_timing;
        memcpy(s_decode_pixel_grb, s_armed_pixel_grb, s_armed_pixel_len);
        s_decode_pixel_len = s_armed_pixel_len;
        ClearPendingArmSignal();
    } else {
        *wait_seq = s_armed_seq;
    }
    xSemaphoreGive(s_rx_lock);
    return is_current;
}

/** @brief Check the symbol count, then decode and log one current capture (FR-29, FR-30, FR-32). */
static void DecodeCapture(const capture_done_event_t *event)
{
    if (event->symbol_count != RMT_PULSE_MONITOR_DATA_SYMBOLS) {
        LOG_WARNING("pulse monitor: symbol count mismatch (reason=symbol_count count=%u)",
                    (unsigned)event->symbol_count);
        return;
    }

    LogBadSymbolsIfAny();

    ws2812_pulse_stats_t stats;
    if (AggregateWs2812Pulses(s_decode_symbols, RMT_PULSE_MONITOR_DATA_SYMBOLS, &s_decode_timing, s_decode_pixel_grb,
                               s_decode_pixel_len, RMT_PULSE_MONITOR_TICK_NS, &stats)) {
        LogPulseStats(&stats);
    }
}

/**
 * @brief Decode task: wait for an armed capture, then for its capture-done event (FR-29 to FR-32).
 *
 * The FR-31 timeout starts only after ArmPulseCapture() has successfully armed
 * a receive, so an idle system never logs a spurious timeout. A stale event
 * (tagged with an older arm) is discarded and the wait continues with a fresh
 * bound for the latest arm; at most one stale event can be pending per older
 * receive, so the wait stays bounded. The sequence number being waited on
 * decides, on timeout, whether the RX restart runs (exactly one Warning per
 * timed-out arm).
 */
static void RunPulseDecodeTask(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_armed_sem, portMAX_DELAY);
        uint32_t wait_seq = GetArmedSeq();

        for (;;) {
            capture_done_event_t event;
            if (xQueueReceive(s_capture_queue, &event, pdMS_TO_TICKS(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS)) != pdTRUE) {
                LOG_WARNING("pulse monitor: capture timed out (reason=no_signal)");
                RestartRxChannel(wait_seq);
                break;
            }
            if (!TakeCurrentCapture(&event, &wait_seq)) {
                LOG_DEBUG("pulse monitor: stale capture discarded (seq=%u)", (unsigned)event.arm_seq);
                continue;
            }
            DecodeCapture(&event);
            break;
        }
    }
}

bool StartPulseMonitor(void)
{
    s_capture_queue = xQueueCreateStatic(1, sizeof(capture_done_event_t), s_capture_queue_storage, &s_capture_queue_struct);
    s_armed_sem = xSemaphoreCreateBinaryStatic(&s_armed_sem_struct);
    s_rx_lock = xSemaphoreCreateMutexStatic(&s_rx_lock_struct);

    rmt_rx_channel_config_t rx_config = {
        .gpio_num = RMT_PULSE_MONITOR_RX_GPIO_NUM,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_PULSE_MONITOR_RESOLUTION_HZ,
        .mem_block_symbols = RMT_PULSE_MONITOR_MEM_BLOCK_SYMBOLS,
    };
    if (rmt_new_rx_channel(&rx_config, &s_rx_channel) != ESP_OK) {
        LOG_ERROR("failed to create RMT RX channel on GPIO %d", RMT_PULSE_MONITOR_RX_GPIO_NUM);
        s_rx_channel = NULL;
        return false;
    }

    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = HandleRxDone};
    if (rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, NULL) != ESP_OK) {
        LOG_ERROR("failed to register RMT RX callback");
        s_rx_channel = NULL;
        return false;
    }

    if (rmt_enable(s_rx_channel) != ESP_OK) {
        LOG_ERROR("failed to enable RMT RX channel");
        s_rx_channel = NULL;
        return false;
    }

    TaskHandle_t handle = xTaskCreateStatic(RunPulseDecodeTask, "pulse_mon", RMT_PULSE_MONITOR_TASK_STACK_BYTES, NULL,
                                             RMT_PULSE_MONITOR_TASK_PRIORITY, s_task_stack, &s_task_struct);
    if (handle == NULL) {
        LOG_ERROR("failed to create pulse monitor decode task");
        rmt_disable(s_rx_channel); /* leave the RX channel disabled (FR-20) */
        s_rx_channel = NULL;
        return false;
    }

    LOG_INFO("pulse monitor started on GPIO %d", RMT_PULSE_MONITOR_RX_GPIO_NUM);
    return true;
}

void ArmPulseCapture(const ws2812_timing_t *applied_timing, const uint8_t *expected_pixel_grb,
                      size_t expected_pixel_len)
{
    if (s_rx_channel == NULL || applied_timing == NULL || expected_pixel_grb == NULL) {
        return; /* StartPulseMonitor() failed or never called: diagnostic-only, no-op (FR-20). */
    }
    if (expected_pixel_len > sizeof(s_armed_pixel_grb)) {
        LOG_WARNING("pulse monitor: expected pixel length %u exceeds capacity", (unsigned)expected_pixel_len);
        return;
    }
    /* Zero-timeout take keeps arming non-blocking (NFR-12): skip this capture while an FR-31 restart
     * or the decode task's bounded snapshot copy holds the lock. */
    if (xSemaphoreTake(s_rx_lock, 0) != pdTRUE) {
        LOG_WARNING("pulse monitor: arm skipped (reason=rx_restart_busy)");
        return;
    }

    /* Tag this receive before starting it, so its done event can never carry an older tag. */
    uint32_t arm_seq = s_armed_seq + 1;
    s_capture_seq = arm_seq;

    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = RMT_PULSE_MONITOR_GLITCH_NS,
        .signal_range_max_ns = RMT_PULSE_MONITOR_IDLE_NS,
    };
    esp_err_t result = rmt_receive(s_rx_channel, s_symbol_buffer, sizeof(s_symbol_buffer), &receive_config);
    if (result == ESP_ERR_INVALID_STATE && rmt_enable(s_rx_channel) == ESP_OK) {
        /* The channel was disabled (an FR-31 restart failed after rmt_disable()): re-enabled once, retry once. */
        LOG_DEBUG("pulse monitor: rx channel re-enabled before arming");
        result = rmt_receive(s_rx_channel, s_symbol_buffer, sizeof(s_symbol_buffer), &receive_config);
    }
    if (result != ESP_OK) {
        s_capture_seq = s_armed_seq; /* a still-pending older receive keeps its own tag */
        xSemaphoreGive(s_rx_lock);
        LOG_WARNING("pulse monitor: arm failed (err=%d)", (int)result);
        return;
    }

    /* Snapshot only after a successful arm, so a still-pending older capture is never paired with it. */
    s_armed_timing = *applied_timing;
    memcpy(s_armed_pixel_grb, expected_pixel_grb, expected_pixel_len);
    s_armed_pixel_len = expected_pixel_len;
    s_armed_seq = arm_seq;
    /* Non-blocking: start the decode task's FR-31 timeout for this capture. */
    xSemaphoreGive(s_armed_sem);
    xSemaphoreGive(s_rx_lock);
}
