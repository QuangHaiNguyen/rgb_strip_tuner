/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file ws2812_timing.c
 * @brief WS2812 timing validation, form parsing and log output (SPEC-003).
 */
#include "ws2812_timing.h"
#include "logging.h"
#include <string.h>

/*
 * Log tag "http_portal" (FR-17 note): the tuner log lines are printed under
 * the http_portal tag because they describe the outcome of an HTTP request
 * handled by that component, even though the formatting lives here.
 */
LOG_MODULE_REGISTER("http_portal", LOG_LEVEL_DEBUG);

/** @brief Longest accepted wire value (4 digits) plus the null terminator. */
#define TUNER_VALUE_BUF_LEN (5)
/** @brief Longest accepted digit count for one wire value (FR-15). */
#define TUNER_VALUE_DIGITS_MAX (4)

/**
 * @brief Check that @p value lies within [@p min, @p max] and is a multiple of @p step.
 */
static bool IsOnStep(uint16_t value, uint16_t min, uint16_t max, uint16_t step)
{
    return value >= min && value <= max && (value % step) == 0;
}

ws2812_timing_result_t ValidateWs2812Timing(const ws2812_timing_t *timing)
{
    bool is_in_range =
        IsOnStep(timing->bit0_high_ns, TUNER_HIGH_MIN_NS, TUNER_HIGH_MAX_NS, TUNER_STEP_NS) &&
        IsOnStep(timing->bit0_period_ns, TUNER_PERIOD_MIN_NS, TUNER_PERIOD_MAX_NS, TUNER_STEP_NS) &&
        IsOnStep(timing->bit1_high_ns, TUNER_HIGH_MIN_NS, TUNER_HIGH_MAX_NS, TUNER_STEP_NS) &&
        IsOnStep(timing->bit1_period_ns, TUNER_PERIOD_MIN_NS, TUNER_PERIOD_MAX_NS, TUNER_STEP_NS) &&
        IsOnStep(timing->reset_us, TUNER_RESET_MIN_US, TUNER_RESET_MAX_US, TUNER_RST_STEP_US);
    if (!is_in_range) {
        return WS2812_TIMING_OUT_OF_RANGE;
    }

    int bit0_low_ns = (int)timing->bit0_period_ns - (int)timing->bit0_high_ns;
    int bit1_low_ns = (int)timing->bit1_period_ns - (int)timing->bit1_high_ns;
    if (bit0_low_ns < TUNER_MIN_LOW_NS || bit1_low_ns < TUNER_MIN_LOW_NS) {
        return WS2812_TIMING_BAD_COMBINATION;
    }
    return WS2812_TIMING_OK;
}

/**
 * @brief Extract the raw (undecoded) digit string for the first occurrence of @p key.
 *
 * @param[in]  body      Null-terminated urlencoded body.
 * @param[in]  key       Wire key to look up, e.g. "b0h_ns".
 * @param[out] out       Receives the null-terminated digit string.
 * @param[in]  out_size  Size of @p out (must be at least TUNER_VALUE_BUF_LEN).
 * @return true if @p key's first occurrence has a 1-to-4-digit numeric value.
 */
static bool ExtractDigitField(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *field = body;
    while (*field != '\0') {
        const char *amp = strchr(field, '&');
        size_t field_len = (amp == NULL) ? strlen(field) : (size_t)(amp - field);
        if (field_len > key_len && strncmp(field, key, key_len) == 0 && field[key_len] == '=') {
            const char *value = field + key_len + 1;
            size_t value_len = field_len - key_len - 1;
            if (value_len < 1 || value_len > TUNER_VALUE_DIGITS_MAX || value_len >= out_size) {
                return false;
            }
            for (size_t index = 0; index < value_len; ++index) {
                if (value[index] < '0' || value[index] > '9') {
                    return false;
                }
            }
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return true;
        }
        if (amp == NULL) {
            break;
        }
        field = amp + 1;
    }
    return false;
}

/** @brief Convert a validated (1-to-4 digit) decimal string to a uint16_t. */
static uint16_t DigitsToUint16(const char *digits)
{
    uint16_t value = 0;
    for (const char *cursor = digits; *cursor != '\0'; ++cursor) {
        value = (uint16_t)(value * 10u + (uint16_t)(*cursor - '0'));
    }
    return value;
}

bool ParseTunerForm(const char *body, ws2812_timing_t *timing)
{
    char bit0_high[TUNER_VALUE_BUF_LEN];
    char bit0_period[TUNER_VALUE_BUF_LEN];
    char bit1_high[TUNER_VALUE_BUF_LEN];
    char bit1_period[TUNER_VALUE_BUF_LEN];
    char reset[TUNER_VALUE_BUF_LEN];

    if (!ExtractDigitField(body, "b0h_ns", bit0_high, sizeof(bit0_high)) ||
        !ExtractDigitField(body, "b0p_ns", bit0_period, sizeof(bit0_period)) ||
        !ExtractDigitField(body, "b1h_ns", bit1_high, sizeof(bit1_high)) ||
        !ExtractDigitField(body, "b1p_ns", bit1_period, sizeof(bit1_period)) ||
        !ExtractDigitField(body, "rst_us", reset, sizeof(reset))) {
        return false;
    }

    timing->bit0_high_ns = DigitsToUint16(bit0_high);
    timing->bit0_period_ns = DigitsToUint16(bit0_period);
    timing->bit1_high_ns = DigitsToUint16(bit1_high);
    timing->bit1_period_ns = DigitsToUint16(bit1_period);
    timing->reset_us = DigitsToUint16(reset);
    return true;
}

void LogWs2812Timing(const ws2812_timing_t *timing)
{
    LOG_INFO("tuner received: bit0 high_ns=%u period_ns=%u; bit1 high_ns=%u period_ns=%u; reset_us=%u",
              (unsigned)timing->bit0_high_ns, (unsigned)timing->bit0_period_ns,
              (unsigned)timing->bit1_high_ns, (unsigned)timing->bit1_period_ns,
              (unsigned)timing->reset_us);
}

void LogWs2812Rejection(ws2812_reject_reason_t reason)
{
    const char *reason_text = "malformed";
    if (reason == WS2812_REJECT_OUT_OF_RANGE) {
        reason_text = "out_of_range";
    } else if (reason == WS2812_REJECT_BAD_COMBINATION) {
        reason_text = "bad_combination";
    }
    LOG_WARNING("tuner request rejected: reason=%s", reason_text);
}
