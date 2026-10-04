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
 *
 * Result publication (FR-34 to FR-37): each armed request's submit_seq travels in
 * the armed snapshot. The decode task publishes done, count_error or timeout for
 * it, and ArmPulseCapture() publishes not_measured whenever it returns without
 * arming. Both publish through the registered callback, outside the ISR and never
 * while s_rx_lock is held; this component knows nothing about the receiver.
 *
 * Read mode (SPEC-006 FR-11 to FR-17): ArmPulseRead() arms the same channel with the
 * capture mode `read` in the armed snapshot. The decode task waits up to
 * RMT_PULSE_MONITOR_READ_TIMEOUT_MS for such an arm (20 ms for a send arm), skips the
 * SPEC-004 count check and decoding, and instead analyzes the capture with
 * AnalyzeWs2812Read(), logs one `pulse read:` line and publishes read_done or
 * count_error. Pairing, stale-event and timeout-restart rules are shared by both modes.
 */
#include "rmt_pulse_monitor.h"
#include "logging.h"
#include <stdio.h>
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

/** @brief What an armed capture measures (SPEC-006 FR-11). */
typedef enum {
    PULSE_CAPTURE_SEND = 0, /**< The ESP's own frame, armed by ArmPulseCapture() (SPEC-004 FR-24). */
    PULSE_CAPTURE_READ,     /**< An external source's frame, armed by ArmPulseRead() (SPEC-006 FR-11). */
} pulse_capture_mode_t;

static rmt_channel_handle_t s_rx_channel;

/* Static capture buffer (FR-23): 576 bytes, filled directly by the RX driver. */
static rmt_symbol_word_t s_symbol_buffer[RMT_PULSE_MONITOR_SYMBOL_CAPACITY];

/* Snapshot written by ArmPulseCapture() (FR-24) under s_rx_lock; copied out by the decode task under s_rx_lock. */
static ws2812_timing_t s_armed_timing;
static uint8_t s_armed_pixel_grb[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES];
static size_t s_armed_pixel_len;
static uint32_t s_armed_submit_seq; /* SPEC-003 FR-23 number of the armed request (FR-35) */
static pulse_capture_mode_t s_armed_mode; /* SPEC-006 FR-11 */
/* Sequence number of the last successful arm; read and written only under s_rx_lock. */
static uint32_t s_armed_seq;
/* Sequence number the ISR tags its done event with. Written only under s_rx_lock; read by the
 * ISR, which cannot take a mutex (a single aligned 32-bit read is atomic on the ESP32-C3). */
static volatile uint32_t s_capture_seq;
/* true while an rmt_receive() is in flight (SPEC-006 FR-13). Set under s_rx_lock just before a receive is
 * started and cleared on its failure; cleared by the ISR when the receive completes and by RestartRxChannel()
 * when it aborts it. A single aligned byte, so the ISR's write needs no lock. An arm finding it set fails
 * at once without calling rmt_receive() or retagging s_capture_seq, so the in-flight receive keeps its tag. */
static volatile bool s_is_receive_pending;

/* Decode task's private copies of one matched capture, taken under s_rx_lock (no sharing afterwards). */
static rmt_symbol_word_t s_decode_symbols[RMT_PULSE_MONITOR_SYMBOL_CAPACITY];
static ws2812_timing_t s_decode_timing;
static uint8_t s_decode_pixel_grb[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES];
static size_t s_decode_pixel_len;
static uint32_t s_decode_submit_seq;
static pulse_capture_mode_t s_decode_mode;

/* Result receiver (FR-34): one aligned word, written once (NULL -> function) and read without a lock. */
static pulse_result_cb_t s_result_cb;

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
 * @brief Check whether symbol @p index of a read capture can be measured (SPEC-006 FR-15 a).
 *
 * The final symbol's duration1 is the RMT end marker, so it is never usable.
 */
static bool IsReadSymbolUsable(rmt_symbol_word_t symbol, size_t index, size_t symbol_count)
{
    return index + 1 < symbol_count && symbol.level0 == 1 && symbol.level1 == 0 && symbol.duration0 > 0 &&
           symbol.duration1 > 0;
}

/** @brief Round-half-up average of @p sum over @p count; 0 for an empty class (SPEC-006 FR-15 d). */
static uint32_t GetRoundedAverage(uint32_t sum, uint32_t count)
{
    return (count > 0) ? (sum + count / 2) / count : 0;
}

bool AnalyzeWs2812Read(const rmt_symbol_word_t *symbols, size_t symbol_count, uint32_t tick_ns,
                       ws2812_read_stats_t *stats)
{
    if (stats == NULL) {
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    if (symbols == NULL) {
        return false;
    }

    uint32_t min_ns = UINT32_MAX;
    uint32_t max_ns = 0;
    for (size_t index = 0; index < symbol_count; ++index) {
        if (!IsReadSymbolUsable(symbols[index], index, symbol_count)) {
            continue;
        }
        uint32_t high_ns = Ws2812TicksToNs(symbols[index].duration0, tick_ns);
        if (high_ns < min_ns) {
            min_ns = high_ns;
        }
        if (high_ns > max_ns) {
            max_ns = high_ns;
        }
        ++stats->usable_count;
    }
    if (stats->usable_count == 0) {
        return false;
    }

    /* Two classes split at the midpoint (tie: bit 0); a narrow spread is one class placed by the datasheet midpoint. */
    bool is_split = (max_ns - min_ns >= RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS);
    bool is_single_bit0 = (min_ns + max_ns <= 2u * RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS);
    uint32_t bit0_high_sum_ns = 0;
    uint32_t bit0_period_sum_ns = 0;
    uint32_t bit1_high_sum_ns = 0;
    uint32_t bit1_period_sum_ns = 0;
    for (size_t index = 0; index < symbol_count; ++index) {
        rmt_symbol_word_t symbol = symbols[index];
        if (!IsReadSymbolUsable(symbol, index, symbol_count)) {
            continue;
        }
        uint32_t high_ns = Ws2812TicksToNs(symbol.duration0, tick_ns);
        uint32_t period_ns = Ws2812TicksToNs((uint32_t)symbol.duration0 + symbol.duration1, tick_ns);
        bool is_bit0 = is_split ? (2u * high_ns <= min_ns + max_ns) : is_single_bit0;
        if (is_bit0) {
            bit0_high_sum_ns += high_ns;
            bit0_period_sum_ns += period_ns;
            ++stats->bit0_count;
        } else {
            bit1_high_sum_ns += high_ns;
            bit1_period_sum_ns += period_ns;
            ++stats->bit1_count;
        }
    }

    stats->bit0_high_avg_ns = GetRoundedAverage(bit0_high_sum_ns, stats->bit0_count);
    stats->bit0_period_avg_ns = GetRoundedAverage(bit0_period_sum_ns, stats->bit0_count);
    stats->bit1_high_avg_ns = GetRoundedAverage(bit1_high_sum_ns, stats->bit1_count);
    stats->bit1_period_avg_ns = GetRoundedAverage(bit1_period_sum_ns, stats->bit1_count);
    return stats->usable_count >= RMT_PULSE_MONITOR_READ_MIN_BITS;
}

/**
 * @brief Minimal ISR-safe hand-off: enqueue the received symbol count, tagged with its arm sequence number (FR-25).
 *
 * Overwrites the 1-deep queue, so an unconsumed older (stale) event can never block the
 * newest one; the decode task tells them apart by the tag. Also marks the channel free
 * to arm again (s_is_receive_pending).
 */
static bool HandleRxDone(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)channel;
    (void)user_ctx;
    capture_done_event_t event = {.symbol_count = edata->num_symbols, .arm_seq = s_capture_seq};
    s_is_receive_pending = false; /* the receive has ended: the channel can be armed again */
    BaseType_t high_task_woken = pdFALSE;
    xQueueOverwriteFromISR(s_capture_queue, &event, &high_task_woken);
    return high_task_woken == pdTRUE;
}

void SetPulseResultCallback(pulse_result_cb_t callback)
{
    s_result_cb = callback;
}

/**
 * @brief Hand one measurement record to the registered callback (FR-34, FR-37); the only reader of the callback pointer.
 *
 * Never called from the ISR or with s_rx_lock held. A NULL callback skips publication.
 *
 * @param[in] measurement Record to publish; valid only during the call.
 */
static void PublishMeasurement(const ws2812_measurement_t *measurement)
{
    pulse_result_cb_t callback = s_result_cb;
    if (callback != NULL) {
        callback(measurement);
    }
}

/**
 * @brief Publish one measurement outcome of a send capture or a failed arm (FR-34, FR-35, FR-36).
 *
 * @param[in] submit_seq Submission sequence number the outcome belongs to.
 * @param[in] state      Outcome.
 * @param[in] stats      Aggregate measurement for WS2812_MEASUREMENT_DONE; NULL otherwise.
 */
static void PublishPulseResult(uint32_t submit_seq, ws2812_measurement_state_t state, const ws2812_pulse_stats_t *stats)
{
    ws2812_measurement_t measurement = {.submit_seq = submit_seq, .state = state};
    if (stats != NULL) {
        measurement.bit0_high_avg_ns = stats->bit0_high_avg_ns;
        measurement.bit1_high_avg_ns = stats->bit1_high_avg_ns;
        measurement.match_count = (uint16_t)stats->match_count;
        measurement.match_available = stats->match_available;
    }
    PublishMeasurement(&measurement);
}

/**
 * @brief Publish one successful read (SPEC-006 FR-17, FR-18). Never called from the ISR or with s_rx_lock held.
 *
 * @param[in] submit_seq Submission sequence number of the Read request.
 * @param[in] stats      Read measurement; averages of 0 mean the bit was not found.
 */
static void PublishReadResult(uint32_t submit_seq, const ws2812_read_stats_t *stats)
{
    ws2812_measurement_t measurement = {
        .submit_seq = submit_seq,
        .state = WS2812_MEASUREMENT_READ_DONE,
        .bit0_high_avg_ns = stats->bit0_high_avg_ns,
        .bit1_high_avg_ns = stats->bit1_high_avg_ns,
        .bit0_period_avg_ns = stats->bit0_period_avg_ns,
        .bit1_period_avg_ns = stats->bit1_period_avg_ns,
    };
    PublishMeasurement(&measurement);
}

/** @brief Longest decimal uint32_t (10 digits) plus the terminator. */
#define PULSE_READ_VALUE_TEXT_MAX (11)

/** @brief Write @p value_ns as decimal, or `n/a` if @p count is 0 (class not found), into @p text. */
static void FormatReadValue(uint32_t value_ns, uint32_t count, char *text)
{
    if (count == 0) {
        strcpy(text, "n/a");
    } else {
        snprintf(text, PULSE_READ_VALUE_TEXT_MAX, "%u", (unsigned)value_ns);
    }
}

/** @brief Log the SPEC-006 FR-16 Info line for one successful read. */
static void LogReadStats(const ws2812_read_stats_t *stats)
{
    char bit0_high[PULSE_READ_VALUE_TEXT_MAX];
    char bit0_period[PULSE_READ_VALUE_TEXT_MAX];
    char bit1_high[PULSE_READ_VALUE_TEXT_MAX];
    char bit1_period[PULSE_READ_VALUE_TEXT_MAX];
    FormatReadValue(stats->bit0_high_avg_ns, stats->bit0_count, bit0_high);
    FormatReadValue(stats->bit0_period_avg_ns, stats->bit0_count, bit0_period);
    FormatReadValue(stats->bit1_high_avg_ns, stats->bit1_count, bit1_high);
    FormatReadValue(stats->bit1_period_avg_ns, stats->bit1_count, bit1_period);
    LOG_INFO("pulse read: bit0 high_ns=%s period_ns=%s; bit1 high_ns=%s period_ns=%s; bits=%u", bit0_high, bit0_period,
             bit1_high, bit1_period, (unsigned)stats->usable_count);
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
 * @brief Read the last successfully armed sequence number, its submit_seq and capture mode under s_rx_lock
 * (FR-31, FR-35, SPEC-006 FR-12).
 *
 * @param[out] submit_seq Receives the submission sequence number of that arm.
 * @param[out] mode       Receives the capture mode of that arm.
 * @return The arm sequence number the decode task's capture wait is for.
 */
static uint32_t GetArmedSeq(uint32_t *submit_seq, pulse_capture_mode_t *mode)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    uint32_t armed_seq = s_armed_seq;
    *submit_seq = s_armed_submit_seq;
    *mode = s_armed_mode;
    xSemaphoreGive(s_rx_lock);
    return armed_seq;
}

/** @brief Capture-done wait bound for an arm of @p mode: 100 ticks for a read, 2 ticks for a send (SPEC-006 FR-12). */
static TickType_t GetCaptureTimeoutTicks(pulse_capture_mode_t mode)
{
    return (mode == PULSE_CAPTURE_READ) ? pdMS_TO_TICKS(RMT_PULSE_MONITOR_READ_TIMEOUT_MS)
                                        : pdMS_TO_TICKS(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS);
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
    /* The timed-out receive is aborted (no done event follows). Cleared even on failure so arming can never
     * stay blocked: a receive that somehow survives makes the next rmt_receive() fail, as before SPEC-006. */
    s_is_receive_pending = false;
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
 * @param[in,out] wait_submit_seq Submission sequence number of that arm; updated with @p wait_seq (FR-31).
 * @param[in,out] wait_mode Capture mode of that arm; updated with @p wait_seq (SPEC-006 FR-12).
 * @return true if the event is current and its data was copied; false if it is stale.
 */
static bool TakeCurrentCapture(const capture_done_event_t *event, uint32_t *wait_seq, uint32_t *wait_submit_seq,
                               pulse_capture_mode_t *wait_mode)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool is_current = (event->arm_seq == s_armed_seq);
    if (is_current) {
        memcpy(s_decode_symbols, s_symbol_buffer, sizeof(s_decode_symbols));
        s_decode_timing = s_armed_timing;
        memcpy(s_decode_pixel_grb, s_armed_pixel_grb, s_armed_pixel_len);
        s_decode_pixel_len = s_armed_pixel_len;
        s_decode_submit_seq = s_armed_submit_seq;
        s_decode_mode = s_armed_mode;
        ClearPendingArmSignal();
    } else {
        *wait_seq = s_armed_seq;
        *wait_submit_seq = s_armed_submit_seq;
        *wait_mode = s_armed_mode;
    }
    xSemaphoreGive(s_rx_lock);
    return is_current;
}

/**
 * @brief Check the symbol count, then decode and log one current capture (FR-29, FR-30, FR-32),
 * and publish its outcome, done or count_error (FR-35).
 */
static void DecodeCapture(const capture_done_event_t *event)
{
    if (event->symbol_count != RMT_PULSE_MONITOR_DATA_SYMBOLS) {
        LOG_WARNING("pulse monitor: symbol count mismatch (reason=symbol_count count=%u)",
                    (unsigned)event->symbol_count);
        PublishPulseResult(s_decode_submit_seq, WS2812_MEASUREMENT_COUNT_ERROR, NULL);
        return;
    }

    LogBadSymbolsIfAny();

    ws2812_pulse_stats_t stats;
    if (AggregateWs2812Pulses(s_decode_symbols, RMT_PULSE_MONITOR_DATA_SYMBOLS, &s_decode_timing, s_decode_pixel_grb,
                               s_decode_pixel_len, RMT_PULSE_MONITOR_TICK_NS, &stats)) {
        LogPulseStats(&stats);
        PublishPulseResult(s_decode_submit_seq, WS2812_MEASUREMENT_DONE, &stats);
    }
}

/**
 * @brief Analyze, log and publish one current read capture: read_done or count_error (SPEC-006 FR-14 to FR-17).
 *
 * No SPEC-004 FR-32 count check, decoding or section 7.6 lines for a read capture.
 */
static void AnalyzeReadCapture(const capture_done_event_t *event)
{
    size_t symbol_count = event->symbol_count;
    if (symbol_count > RMT_PULSE_MONITOR_SYMBOL_CAPACITY) {
        symbol_count = RMT_PULSE_MONITOR_SYMBOL_CAPACITY; /* never past the copied buffer */
    }
    ws2812_read_stats_t stats;
    if (!AnalyzeWs2812Read(s_decode_symbols, symbol_count, RMT_PULSE_MONITOR_TICK_NS, &stats)) {
        LOG_WARNING("pulse read: too few bits (count=%u)", (unsigned)stats.usable_count);
        PublishPulseResult(s_decode_submit_seq, WS2812_MEASUREMENT_COUNT_ERROR, NULL);
        return;
    }
    LogReadStats(&stats);
    PublishReadResult(s_decode_submit_seq, &stats);
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
 * timed-out arm). A timeout publishes `timeout` for the submit_seq of the arm
 * waited on, after the restart has released s_rx_lock (FR-35, FR-37); a stale
 * arm publishes nothing. The wait bound follows the capture mode of the arm waited
 * on (SPEC-006 FR-12), and a current capture is decoded or analyzed by its mode.
 */
static void RunPulseDecodeTask(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_armed_sem, portMAX_DELAY);
        uint32_t wait_submit_seq = 0;
        pulse_capture_mode_t wait_mode = PULSE_CAPTURE_SEND;
        uint32_t wait_seq = GetArmedSeq(&wait_submit_seq, &wait_mode);

        for (;;) {
            capture_done_event_t event;
            if (xQueueReceive(s_capture_queue, &event, GetCaptureTimeoutTicks(wait_mode)) != pdTRUE) {
                LOG_WARNING("pulse monitor: capture timed out (reason=no_signal)");
                RestartRxChannel(wait_seq);
                PublishPulseResult(wait_submit_seq, WS2812_MEASUREMENT_TIMEOUT, NULL);
                break;
            }
            if (!TakeCurrentCapture(&event, &wait_seq, &wait_submit_seq, &wait_mode)) {
                LOG_DEBUG("pulse monitor: stale capture discarded (seq=%u)", (unsigned)event.arm_seq);
                continue;
            }
            if (s_decode_mode == PULSE_CAPTURE_READ) {
                AnalyzeReadCapture(&event);
            } else {
                DecodeCapture(&event);
            }
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

/**
 * @brief Start one tagged rmt_receive() into the capture buffer (FR-24, shared by both capture modes).
 *
 * The caller holds s_rx_lock. While a receive is in flight it returns ESP_ERR_INVALID_STATE
 * at once, without calling rmt_receive() and without retagging, so the in-flight receive's
 * done event keeps its own tag (SPEC-006 FR-13). If the channel is disabled (a failed FR-31
 * restart), it is re-enabled once and the receive retried once. On failure the ISR tag and
 * the pending flag are restored, and the Warning is left to the caller.
 *
 * @param[out] arm_seq Receives the new arm sequence number on success.
 * @return ESP_OK, ESP_ERR_INVALID_STATE for a receive in flight, or the last rmt_receive() result.
 */
static esp_err_t StartTaggedReceive(uint32_t *arm_seq)
{
    if (s_is_receive_pending) {
        return ESP_ERR_INVALID_STATE; /* the same code rmt_receive() reports for a busy channel */
    }
    /* Tag this receive before starting it, so its done event can never carry an older tag. The flag is set
     * first too: a done ISR for this receive may run before rmt_receive() returns, and must leave it false. */
    *arm_seq = s_armed_seq + 1;
    s_capture_seq = *arm_seq;
    s_is_receive_pending = true;

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
        s_is_receive_pending = false;
    }
    return result;
}

void ArmPulseCapture(const ws2812_timing_t *applied_timing, uint32_t submit_seq, const uint8_t *expected_pixel_grb,
                      size_t expected_pixel_len)
{
    /* Every return without arming publishes not_measured, with s_rx_lock not held (FR-36, FR-37). */
    if (s_rx_channel == NULL || applied_timing == NULL || expected_pixel_grb == NULL) {
        /* StartPulseMonitor() failed or never called: diagnostic-only, no capture (FR-20). */
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }
    if (expected_pixel_len > sizeof(s_armed_pixel_grb)) {
        LOG_WARNING("pulse monitor: expected pixel length %u exceeds capacity", (unsigned)expected_pixel_len);
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }
    /* Zero-timeout take keeps arming non-blocking (NFR-12): skip this capture while an FR-31 restart
     * or the decode task's bounded snapshot copy holds the lock. */
    if (xSemaphoreTake(s_rx_lock, 0) != pdTRUE) {
        LOG_WARNING("pulse monitor: arm skipped (reason=rx_restart_busy)");
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }

    uint32_t arm_seq = 0;
    esp_err_t result = StartTaggedReceive(&arm_seq);
    if (result != ESP_OK) {
        xSemaphoreGive(s_rx_lock);
        LOG_WARNING("pulse monitor: arm failed (err=%d)", (int)result);
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }

    /* Snapshot only after a successful arm, so a still-pending older capture is never paired with it. */
    s_armed_timing = *applied_timing;
    memcpy(s_armed_pixel_grb, expected_pixel_grb, expected_pixel_len);
    s_armed_pixel_len = expected_pixel_len;
    s_armed_submit_seq = submit_seq;
    s_armed_mode = PULSE_CAPTURE_SEND;
    s_armed_seq = arm_seq;
    /* Non-blocking: start the decode task's FR-31 timeout for this capture. */
    xSemaphoreGive(s_armed_sem);
    xSemaphoreGive(s_rx_lock);
}

void ArmPulseRead(uint32_t submit_seq)
{
    /* Every return without arming publishes not_measured, with s_rx_lock not held (SPEC-006 FR-11). */
    if (s_rx_channel == NULL) {
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }
    if (xSemaphoreTake(s_rx_lock, 0) != pdTRUE) {
        LOG_WARNING("pulse monitor: arm skipped (reason=rx_restart_busy)");
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }

    uint32_t arm_seq = 0;
    esp_err_t result = StartTaggedReceive(&arm_seq);
    if (result != ESP_OK) {
        xSemaphoreGive(s_rx_lock);
        LOG_WARNING("pulse monitor: arm failed (err=%d)", (int)result);
        PublishPulseResult(submit_seq, WS2812_MEASUREMENT_NOT_MEASURED, NULL);
        return;
    }

    /* A read stores no timing or pixel data: only its mode and number (SPEC-006 FR-11). */
    s_armed_submit_seq = submit_seq;
    s_armed_mode = PULSE_CAPTURE_READ;
    s_armed_seq = arm_seq;
    /* Non-blocking: start the decode task's read timeout for this capture (SPEC-006 FR-12). */
    xSemaphoreGive(s_armed_sem);
    xSemaphoreGive(s_rx_lock);
}
