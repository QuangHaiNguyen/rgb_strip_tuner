/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file ws2812_timing.h
 * @brief WS2812 bit-timing data type, limits, validation and logging (SPEC-003).
 *
 * Pure logic: parsing, validation and log formatting have no ESP-IDF
 * dependency other than the logging header, so this component can be tested
 * on the host with Catch2 + FFF (NFR-15).
 *
 * Also defines the compact measurement record (ws2812_measurement_t, SPEC-003
 * section 7.6) shared by rmt_pulse_monitor, provisioning and http_portal.
 */
#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Editable step of the four bit-timing (ns) values (section 7.1). */
#define TUNER_STEP_NS (25)
/** @brief Editable step of the reset time, in microseconds (section 7.1). */
#define TUNER_RST_STEP_US (10)
/** @brief Minimum accepted low time (period - high) for either bit, in ns (rule V3). */
#define TUNER_MIN_LOW_NS (100)

/** @brief Bit-0/bit-1 high-time (pulse-width) range, in nanoseconds (section 7.1). */
#define TUNER_HIGH_MIN_NS (100)
#define TUNER_HIGH_MAX_NS (1200)
/** @brief Bit-0/bit-1 period range, in nanoseconds (section 7.1). */
#define TUNER_PERIOD_MIN_NS (800)
#define TUNER_PERIOD_MAX_NS (2000)
/** @brief Reset (latch) time range, in microseconds (section 7.1). */
#define TUNER_RESET_MIN_US (50)
#define TUNER_RESET_MAX_US (800)

/** @brief Default bit-0 high time and period, matching the WS2812B datasheet (section 7.1). */
#define TUNER_DEFAULT_BIT0_HIGH_NS (400)
#define TUNER_DEFAULT_BIT0_PERIOD_NS (1250)
/** @brief Default bit-1 high time and period (section 7.1). */
#define TUNER_DEFAULT_BIT1_HIGH_NS (800)
#define TUNER_DEFAULT_BIT1_PERIOD_NS (1250)
/** @brief Default reset time, in microseconds (section 7.1). */
#define TUNER_DEFAULT_RESET_US (280)

/** @brief Largest accepted `POST /tuner` request body, in bytes (FR-15). */
#define TUNER_BODY_MAX (96)
/** @brief Largest served `GET /tuner` response body, in bytes (NFR-2). */
#define TUNER_PAGE_MAX_BYTES (4096)

/**
 * @brief The five canonical WS2812 timing values (section 7.2), 10 bytes.
 *
 * Low time and duty cycle are derived, page-side-only values and are neither
 * stored here nor transmitted nor logged by the firmware.
 */
typedef struct {
    uint16_t bit0_high_ns;    /**< Bit-0 high time (pulse width), ns. */
    uint16_t bit0_period_ns;  /**< Bit-0 period (high + low), ns. */
    uint16_t bit1_high_ns;    /**< Bit-1 high time (pulse width), ns. */
    uint16_t bit1_period_ns;  /**< Bit-1 period (high + low), ns. */
    uint16_t reset_us;        /**< Reset (latch) time, us. */
} ws2812_timing_t;

/** @brief Result of ValidateWs2812Timing() (section 7.3). */
typedef enum {
    WS2812_TIMING_OK = 0,          /**< All five values are in range, on-step, and rules V3 and V4 hold. */
    WS2812_TIMING_OUT_OF_RANGE,    /**< Rule V1 or V2 failed. */
    WS2812_TIMING_BAD_COMBINATION, /**< Rule V3 failed: a bit's low time is below TUNER_MIN_LOW_NS. */
    WS2812_TIMING_BAD_DUTY_ORDER,  /**< Rule V4 failed: bit-0 duty is not strictly below bit-1 duty. */
} ws2812_timing_result_t;

/** @brief Reason reported by LogWs2812Rejection() and the `400` response body (FR-18). */
typedef enum {
    WS2812_REJECT_MALFORMED = 0,   /**< ParseTunerForm() failed: body/keys/digits invalid. */
    WS2812_REJECT_OUT_OF_RANGE,    /**< ValidateWs2812Timing() returned WS2812_TIMING_OUT_OF_RANGE. */
    WS2812_REJECT_BAD_COMBINATION, /**< ValidateWs2812Timing() returned WS2812_TIMING_BAD_COMBINATION. */
    WS2812_REJECT_BAD_DUTY_ORDER,  /**< ValidateWs2812Timing() returned WS2812_TIMING_BAD_DUTY_ORDER. */
} ws2812_reject_reason_t;

/** @brief Outcome of one pulse measurement (SPEC-003 section 7.6, SPEC-004 FR-35/FR-36). */
typedef enum {
    WS2812_MEASUREMENT_DONE = 0,     /**< 144 symbols captured and aggregated; averages and match are valid. */
    WS2812_MEASUREMENT_TIMEOUT,      /**< The armed capture timed out (SPEC-004 FR-31). */
    WS2812_MEASUREMENT_COUNT_ERROR,  /**< The capture had a symbol count other than 144 (SPEC-004 FR-32). */
    WS2812_MEASUREMENT_NOT_MEASURED, /**< The capture could not be armed (SPEC-004 FR-36). */
} ws2812_measurement_state_t;

/**
 * @brief Compact measurement result of one transmitted request (SPEC-003 section 7.6), 20 bytes.
 *
 * Published by rmt_pulse_monitor, forwarded by the provisioning orchestrator, and
 * stored by http_portal for `GET /tuner/result`.
 */
typedef struct {
    uint32_t submit_seq;                /**< Submission sequence number (SPEC-003 FR-23); 0 = boot frame. */
    ws2812_measurement_state_t state;   /**< Outcome of the measurement. */
    uint32_t bit0_high_avg_ns;          /**< Average measured bit-0 high time, ns; WS2812_MEASUREMENT_DONE only. */
    uint32_t bit1_high_avg_ns;          /**< Average measured bit-1 high time, ns; WS2812_MEASUREMENT_DONE only. */
    uint16_t match_count;               /**< Decoded bits matching the GRB pattern, 0..144; valid if match_available. */
    bool match_available;               /**< false if the bits cannot be told apart (equal commanded high times). */
} ws2812_measurement_t;

static_assert(sizeof(ws2812_measurement_t) == 20, "ws2812_measurement_t must be 20 bytes (SPEC-003 section 7.6)");

/**
 * @brief Validate a timing set against section 7.3 rules V1-V4, in rule order.
 *
 * A pure function of the five integers, independent of the client. The first
 * failing rule decides the result. V4 orders the duty cycles, not the high
 * times: bit-0 duty must be strictly below bit-1 duty, checked as the exact
 * product comparison `b0h_ns * b1p_ns < b1h_ns * b0p_ns` with no division.
 *
 * @param[in] timing Timing set to validate.
 * @return WS2812_TIMING_OK, WS2812_TIMING_OUT_OF_RANGE, WS2812_TIMING_BAD_COMBINATION,
 *         or WS2812_TIMING_BAD_DUTY_ORDER.
 */
ws2812_timing_result_t ValidateWs2812Timing(const ws2812_timing_t *timing);

/**
 * @brief Parse a `POST /tuner` body into a timing set (FR-14, FR-15).
 *
 * Looks up the first occurrence of each of the five wire keys (`b0h_ns`,
 * `b0p_ns`, `b1h_ns`, `b1p_ns`, `rst_us`); any other key is ignored. A value
 * is accepted only if it is 1 to 4 ASCII digits ('0'-'9') with no sign,
 * percent-escape, or other character; leading zeros are accepted within the
 * 4-digit limit. Percent-escapes are not decoded, so an escaped value is
 * always treated as non-numeric (FR-15 note).
 *
 * @param[in]  body   Null-terminated `application/x-www-form-urlencoded` body.
 * @param[out] timing Receives the parsed values on success.
 * @return true if all five keys were present with a valid numeric value; false otherwise.
 */
bool ParseTunerForm(const char *body, ws2812_timing_t *timing);

/**
 * @brief Print the FR-17 Info-level log line for an accepted timing set.
 *
 * Prints only bit-0 and bit-1 high time and period and the reset time; no
 * low time and no duty cycle are printed (the firmware derives neither).
 *
 * @param[in] timing Accepted timing set.
 */
void LogWs2812Timing(const ws2812_timing_t *timing);

/**
 * @brief Print the FR-18 Warning-level log line for a rejected `POST /tuner` request.
 *
 * @param[in] reason Rejection reason.
 */
void LogWs2812Rejection(ws2812_reject_reason_t reason);

#ifdef __cplusplus
}
#endif
