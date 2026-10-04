/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file rmt_pulse_monitor.h
 * @brief RMT RX pulse measurement of the WS2812 waveform led_controller transmits (SPEC-004 section 3.6).
 *
 * Captures the exact frame transmitted on GPIO8, by default over an external
 * jumper wire to GPIO4 (a second, independent RMT RX channel, distinct from
 * led_controller's TX channel), decodes it back to bit-0/bit-1
 * classifications, and prints a compact, bounded set of Info-level lines to
 * the terminal (section 7.6). The trailing reset low is not measured: it is
 * contiguous with the last data bit's low, and the RX idle threshold ends the
 * capture, so only the 144 data-bit symbols are received (FR-23). Diagnostic only: never
 * gates, blocks, retries, persists, or feeds back into any control loop or
 * UI (section 1.2).
 *
 * This component has no dependency on led_controller (NFR-17): the
 * dependency runs the other way (led_controller calls ArmPulseCapture()).
 * It has no dependency on http_portal or provisioning either: one compact
 * result per transmitted request is published through the callback set with
 * SetPulseResultCallback() (FR-34 to FR-37).
 *
 * Read mode (SPEC-006): ArmPulseRead() arms the same RX channel to capture a
 * frame that an external WS2812 controller drives into GPIO4, with no
 * transmit and no expected data. The decode task then classifies the pulses by
 * the midpoint of the shortest and longest high time (AnalyzeWs2812Read()) and
 * publishes the average high time and period of each bit.
 *
 * Ws2812TicksToNs(), DecodeWs2812Symbol(), AggregateWs2812Pulses() and
 * AnalyzeWs2812Read() are pure functions with no ESP-IDF/RMT-driver dependency
 * (NFR-14, SPEC-006 NFR-8); all RMT RX hardware interaction is exercised only
 * through StartPulseMonitor(), ArmPulseCapture() and ArmPulseRead().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/rmt_types.h"
#include "ws2812_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Default/primary RX GPIO, jumper-wired to LED_CONTROLLER_GPIO_NUM (FR-21). */
#define RMT_PULSE_MONITOR_RX_GPIO_NUM (4)
/** @brief RX channel resolution, in Hz; matches led_controller's TX channel (FR-19b). */
#define RMT_PULSE_MONITOR_RESOLUTION_HZ (40000000)
/** @brief RX channel tick duration, in ns (1e9 / RMT_PULSE_MONITOR_RESOLUTION_HZ). */
#define RMT_PULSE_MONITOR_TICK_NS (25)
/** @brief Glitch filter, signal_range_min_ns, in ns (FR-22). */
#define RMT_PULSE_MONITOR_GLITCH_NS (50)
/** @brief End-of-packet threshold, signal_range_max_ns, in ns (FR-22). */
#define RMT_PULSE_MONITOR_IDLE_NS (25000)
/** @brief Number of data-bit symbols in one captured frame: 6 LEDs x 24 bits (FR-23). */
#define RMT_PULSE_MONITOR_DATA_SYMBOLS (144)
/**
 * @brief Capture buffer capacity, in symbols (FR-23).
 *
 * Exactly the expected symbol count, no headroom. The IDF RX driver copies
 * only the symbols actually received and truncates silently at a full buffer
 * (no overflow error), so a longer foreign signal also reports count=144: the
 * FR-32 count check only catches short captures.
 */
#define RMT_PULSE_MONITOR_SYMBOL_CAPACITY (RMT_PULSE_MONITOR_DATA_SYMBOLS)
/** @brief Capture buffer size, in bytes (144 * sizeof(rmt_symbol_word_t) = 576). */
#define RMT_PULSE_MONITOR_BUFFER_BYTES (RMT_PULSE_MONITOR_SYMBOL_CAPACITY * sizeof(rmt_symbol_word_t))
/** @brief Decode task's capture-done wait bound after a successful arm, in ms (FR-31); 2 ticks at 100 Hz. */
#define RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS (20)
/** @brief Largest expected-pixel snapshot this component stores (FR-24), matching led_controller's 18-byte pattern. */
#define RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES (18)
/** @brief Decode task's capture-done wait bound after a successful read arm, in ms (SPEC-006 FR-12); 100 ticks at 100 Hz. */
#define RMT_PULSE_MONITOR_READ_TIMEOUT_MS (1000)
/** @brief Minimum usable symbols for a valid read capture (SPEC-006 FR-14, FR-15). */
#define RMT_PULSE_MONITOR_READ_MIN_BITS (8)
/** @brief Minimum spread of the measured high times for two bit classes, in ns (SPEC-006 FR-15). */
#define RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS (100)
/** @brief High-time threshold for a single-class read capture, in ns: WS2812B T0H/T1H midpoint (SPEC-006 FR-15). */
#define RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS (625)

/** @brief Result of classifying one captured RMT symbol (FR-27). */
typedef enum {
    WS2812_SYMBOL_BIT0 = 0, /**< high_ns nearer the commanded bit-0 high time, or exactly equidistant (tie). */
    WS2812_SYMBOL_BIT1,     /**< high_ns strictly nearer the commanded bit-1 high time. */
    WS2812_SYMBOL_INVALID,  /**< level0 != 1: not a data-bit symbol. */
} ws2812_symbol_class_t;

/** @brief Aggregate measurement of one successfully captured frame (FR-28). */
typedef struct {
    uint32_t bit0_first_high_ns; /**< High time of the first bit-0 symbol with a measurable low time (not the final symbol). */
    uint32_t bit0_first_low_ns;  /**< Low time of that bit-0 symbol; 0 if no such symbol exists. */
    uint32_t bit1_first_high_ns; /**< High time of the first bit-1 symbol with a measurable low time (not the final symbol). */
    uint32_t bit1_first_low_ns;  /**< Low time of that bit-1 symbol; 0 if no such symbol exists. */
    uint32_t bit0_high_min_ns;   /**< Minimum high time across all bit-0 symbols. */
    uint32_t bit0_high_max_ns;   /**< Maximum high time across all bit-0 symbols. */
    uint32_t bit0_high_avg_ns;   /**< Average (round-half-up) high time across all bit-0 symbols. */
    uint32_t bit1_high_min_ns;   /**< Minimum high time across all bit-1 symbols. */
    uint32_t bit1_high_max_ns;   /**< Maximum high time across all bit-1 symbols. */
    uint32_t bit1_high_avg_ns;   /**< Average (round-half-up) high time across all bit-1 symbols. */
    uint32_t match_count;        /**< Decoded bits matching the expected GRB pattern, out of RMT_PULSE_MONITOR_DATA_SYMBOLS; 0 if not available. */
    bool match_available;        /**< false if the commanded bit-0 and bit-1 high times are equal: bits cannot be told apart. */
} ws2812_pulse_stats_t;

/**
 * @brief Measured timing of one read capture of an external source (SPEC-006 FR-15).
 *
 * A bit class with count 0 was not found and reports 0 for both averages.
 */
typedef struct {
    uint32_t usable_count;       /**< Usable symbols (both classes together). */
    uint32_t bit0_count;         /**< Usable symbols classified as bit 0. */
    uint32_t bit0_high_avg_ns;   /**< Average (round-half-up) bit-0 high time; 0 = not found. */
    uint32_t bit0_period_avg_ns; /**< Average (round-half-up) bit-0 period (high + low); 0 = not found. */
    uint32_t bit1_count;         /**< Usable symbols classified as bit 1. */
    uint32_t bit1_high_avg_ns;   /**< Average (round-half-up) bit-1 high time; 0 = not found. */
    uint32_t bit1_period_avg_ns; /**< Average (round-half-up) bit-1 period (high + low); 0 = not found. */
} ws2812_read_stats_t;

/**
 * @brief Receiver of published measurement results (FR-34).
 *
 * Called from the decode task, from ArmPulseCapture() (led_controller's driver task), or from
 * ArmPulseRead() (the provisioning orchestrator task, not_measured only, SPEC-006 FR-11),
 * never from an ISR and never while the RX-channel mutex is held (FR-37). Must not block.
 *
 * @param[in] measurement Result for one request; valid only during the call.
 */
typedef void (*pulse_result_cb_t)(const ws2812_measurement_t *measurement);

/**
 * @brief Convert an RMT tick count to nanoseconds (FR-26).
 *
 * Pure function, the exact inverse of Ws2812NsToTicks() (led_controller.h) at
 * tick_ns = 25 for every tick count this feature produces. No ESP-IDF/RMT
 * dependency (NFR-14).
 *
 * @param[in] ticks   Tick count.
 * @param[in] tick_ns Duration of one tick, in nanoseconds.
 * @return Duration in nanoseconds.
 */
uint32_t Ws2812TicksToNs(uint32_t ticks, uint32_t tick_ns);

/**
 * @brief Classify one captured RMT symbol as bit 0, bit 1, or unclassifiable (FR-27).
 *
 * Pure function. The symbol is the bit whose commanded (not measured) high
 * time is nearer to the measured high time; this also holds when the bit-0
 * high time exceeds the bit-1 high time (inverted timing). Tie rule: an
 * exactly equidistant pulse is bit 0 (so with equal commanded high times every
 * symbol is bit 0). No ESP-IDF/RMT dependency (NFR-14).
 *
 * @param[in] symbol       Captured symbol.
 * @param[in] bit0_high_ns Commanded bit-0 high time of the applied timing set.
 * @param[in] bit1_high_ns Commanded bit-1 high time of the applied timing set.
 * @param[in] tick_ns      Duration of one tick, in nanoseconds.
 * @return WS2812_SYMBOL_BIT0/BIT1, or WS2812_SYMBOL_INVALID if level0 != 1.
 */
ws2812_symbol_class_t DecodeWs2812Symbol(rmt_symbol_word_t symbol, uint32_t bit0_high_ns, uint32_t bit1_high_ns,
                                          uint32_t tick_ns);

/**
 * @brief Aggregate a full capture into the FR-28 measurement.
 *
 * Pure function. Symbols classified WS2812_SYMBOL_INVALID by
 * DecodeWs2812Symbol() are excluded from the high-time statistics and
 * counted as a mismatch in the match/mismatch comparison. The final symbol's
 * duration1 is the RMT end marker, not a measured low time, so the final
 * symbol never supplies a first-bit example; it still counts toward the
 * high-time statistics and the match count.
 *
 * If the applied bit-0 and bit-1 high times are equal, the bits cannot be told
 * apart from timing: stats->match_available is false, match_count is 0, and the
 * bit-0/bit-1 statistics partition the symbols by the expected bit from
 * @p expected_pixel_grb instead of the decoded bit. No ESP-IDF/RMT dependency (NFR-14).
 *
 * @param[in]  symbols            Captured symbols; exactly RMT_PULSE_MONITOR_DATA_SYMBOLS (144) entries.
 * @param[in]  symbol_count       Number of entries in @p symbols; must equal RMT_PULSE_MONITOR_DATA_SYMBOLS.
 * @param[in]  applied_timing     Timing set that was applied for this frame.
 * @param[in]  expected_pixel_grb Expected GRB pixel buffer transmitted for this frame (FR-24), MSB-first per byte.
 * @param[in]  expected_pixel_len Length of @p expected_pixel_grb, in bytes.
 * @param[in]  tick_ns            Duration of one tick, in nanoseconds.
 * @param[out] stats              Receives the aggregate measurement.
 * @return true on success; false if an argument is NULL or @p symbol_count is wrong.
 */
bool AggregateWs2812Pulses(const rmt_symbol_word_t *symbols, size_t symbol_count, const ws2812_timing_t *applied_timing,
                           const uint8_t *expected_pixel_grb, size_t expected_pixel_len, uint32_t tick_ns,
                           ws2812_pulse_stats_t *stats);

/**
 * @brief Measure the average high time and period of each bit of a read capture (SPEC-006 FR-15).
 *
 * Pure function. A symbol is usable only if it is not the final symbol (whose
 * low time is the RMT end marker), has level0 = 1 and level1 = 0, and both
 * durations are non-zero. If the longest and shortest usable high times differ
 * by at least RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS, a symbol is bit 0 when
 * 2 x high <= shortest + longest (the midpoint itself is bit 0), otherwise bit 1.
 * With a smaller spread all usable symbols form one class: bit 0 if
 * shortest + longest <= 2 x RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS, otherwise
 * bit 1. Averages are rounded half up; a class with no symbol reports 0. All
 * arithmetic is integer. No ESP-IDF/RMT dependency (SPEC-006 NFR-8).
 *
 * @param[in]  symbols      Captured symbols; may be NULL only if @p symbol_count is 0.
 * @param[in]  symbol_count Number of entries in @p symbols.
 * @param[in]  tick_ns      Duration of one tick, in nanoseconds.
 * @param[out] stats        Receives the measurement (zeroed first); must not be NULL.
 * @return true if at least RMT_PULSE_MONITOR_READ_MIN_BITS symbols are usable.
 */
bool AnalyzeWs2812Read(const rmt_symbol_word_t *symbols, size_t symbol_count, uint32_t tick_ns,
                       ws2812_read_stats_t *stats);

/**
 * @brief Configure GPIO4, create and enable the RMT RX channel, and start the decode task (FR-19).
 *
 * Call once from app_main(), before StartLedController(), so the RX channel
 * is armable before the boot-time default frame is transmitted. On failure,
 * logs at Error level and returns false without preventing the rest of
 * app_main() from running, and without ever gating led_controller (FR-20).
 *
 * @return true if the RX channel and decode task started.
 */
bool StartPulseMonitor(void);

/**
 * @brief Set the receiver of published measurement results (FR-34).
 *
 * Call once, before results are expected (the provisioning orchestrator does so in
 * ProvisioningStart()). The pointer is a single aligned word read without a lock; an
 * outcome produced before registration is not published. NULL disables publication.
 *
 * @param[in] callback Non-blocking result receiver, or NULL.
 */
void SetPulseResultCallback(pulse_result_cb_t callback);

/**
 * @brief Arm one capture immediately before led_controller's rmt_transmit() call (FR-24).
 *
 * Non-blocking: starts one rmt_receive() job tagged with a new arm sequence
 * number, then copies @p applied_timing, @p submit_seq and @p expected_pixel_grb
 * into static storage and returns without waiting for the capture to complete. If
 * rmt_receive() reports ESP_ERR_INVALID_STATE because the channel is disabled
 * (a failed FR-31 restart), the channel is re-enabled once and the receive
 * retried once. A no-op (with a Warning log) if StartPulseMonitor() failed, a
 * previous capture is still pending, the RX lock is busy (taken with zero
 * timeout: an FR-31 restart or the decode task's snapshot copy holds it), or
 * @p expected_pixel_len exceeds RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES.
 * Only a successful arm starts the decode task's capture timeout (FR-31).
 * Every return without arming publishes WS2812_MEASUREMENT_NOT_MEASURED for
 * @p submit_seq, after releasing the RX-channel mutex (FR-36, FR-37).
 *
 * @param[in] applied_timing     Timing set about to be transmitted.
 * @param[in] submit_seq         Submission sequence number of the request (SPEC-003 FR-23); 0 for the boot frame.
 * @param[in] expected_pixel_grb Pixel buffer about to be transmitted.
 * @param[in] expected_pixel_len Length of @p expected_pixel_grb, in bytes.
 */
void ArmPulseCapture(const ws2812_timing_t *applied_timing, uint32_t submit_seq, const uint8_t *expected_pixel_grb,
                      size_t expected_pixel_len);

/**
 * @brief Arm one read capture of an external WS2812 source on GPIO4 (SPEC-006 FR-11).
 *
 * Arms exactly like ArmPulseCapture() (zero-timeout RX lock take, one
 * rmt_receive(), one re-enable and retry if the channel is disabled, a new arm
 * sequence number, no wait), but transmits nothing and stores only the capture
 * mode `read` and @p submit_seq. The decode task then waits up to
 * RMT_PULSE_MONITOR_READ_TIMEOUT_MS and publishes WS2812_MEASUREMENT_READ_DONE,
 * WS2812_MEASUREMENT_COUNT_ERROR or WS2812_MEASUREMENT_TIMEOUT (SPEC-006 FR-12 to
 * FR-17). Every return without arming publishes WS2812_MEASUREMENT_NOT_MEASURED for
 * @p submit_seq, after releasing the RX-channel mutex. Never touches GPIO8 (SPEC-006 FR-9).
 *
 * @param[in] submit_seq Submission sequence number of the Read request (SPEC-003 FR-23).
 */
void ArmPulseRead(uint32_t submit_seq);

#ifdef __cplusplus
}
#endif
