/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file tuner_result.c
 * @brief Pure helpers for `GET /tuner/result` (SPEC-003 FR-25, FR-26, section 7.6; SPEC-006 FR-19).
 *
 * Query parsing, state decision and body formatting, with no ESP-IDF dependency,
 * so they can be tested on the host. The handler, the record and its mutex live
 * in http_portal.c.
 */
#include "http_portal.h"
#include <stdio.h>
#include <string.h>

/** @brief Name of the query key holding the submission sequence number. */
#define TUNER_RESULT_SEQ_KEY "seq"
/** @brief Longest accepted `seq` value, in digits (4,294,967,295). */
#define TUNER_RESULT_SEQ_DIGITS_MAX (10)
/** @brief One read-body value: a decimal uint32_t (10 digits) or `n/a`, plus the terminator. */
#define TUNER_RESULT_VALUE_TEXT_MAX (11)

/**
 * @brief Convert a field value of 1 to TUNER_RESULT_SEQ_DIGITS_MAX digits to a number of at most UINT32_MAX.
 *
 * @param[in]  value        Value text (not null-terminated).
 * @param[in]  value_length Length of @p value.
 * @param[out] seq          Receives the number on success.
 * @return true if the value is well-formed.
 */
static bool ParseSeqDigits(const char *value, size_t value_length, uint32_t *seq)
{
    if (value_length < 1 || value_length > TUNER_RESULT_SEQ_DIGITS_MAX) {
        return false;
    }
    uint64_t number = 0;
    for (size_t index = 0; index < value_length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        number = number * 10u + (uint64_t)(value[index] - '0');
    }
    if (number > UINT32_MAX) {
        return false;
    }
    *seq = (uint32_t)number;
    return true;
}

bool ParseTunerResultSeq(const char *query, uint32_t *seq)
{
    if (query == NULL || seq == NULL) {
        return false;
    }
    const size_t key_length = sizeof(TUNER_RESULT_SEQ_KEY) - 1;
    const char *field = query;
    for (;;) {
        size_t field_length = strcspn(field, "&");
        size_t name_length = strcspn(field, "=&");
        if (name_length == key_length && strncmp(field, TUNER_RESULT_SEQ_KEY, key_length) == 0) {
            if (name_length == field_length) {
                return false; /* `seq` without a value */
            }
            return ParseSeqDigits(field + key_length + 1, field_length - key_length - 1, seq);
        }
        if (field[field_length] == '\0') {
            return false; /* no `seq` key */
        }
        field += field_length + 1;
    }
}

tuner_result_state_t GetTunerResultState(uint32_t request_seq, uint32_t last_issued_seq,
                                         const ws2812_measurement_t *record)
{
    if (request_seq == 0 || request_seq > last_issued_seq) {
        return TUNER_RESULT_UNKNOWN;
    }
    if (record->submit_seq == request_seq) {
        switch (record->state) {
        case WS2812_MEASUREMENT_DONE: return TUNER_RESULT_DONE;
        case WS2812_MEASUREMENT_TIMEOUT: return TUNER_RESULT_TIMEOUT;
        case WS2812_MEASUREMENT_COUNT_ERROR: return TUNER_RESULT_COUNT_ERROR;
        case WS2812_MEASUREMENT_NOT_MEASURED: return TUNER_RESULT_NOT_MEASURED;
        case WS2812_MEASUREMENT_READ_DONE: return TUNER_RESULT_READ;
        }
        return TUNER_RESULT_PENDING; /* not reached: every published state is handled above */
    }
    if (record->submit_seq > request_seq) {
        return TUNER_RESULT_SUPERSEDED;
    }
    return TUNER_RESULT_PENDING;
}

/**
 * @brief Write one read-body value: @p value_ns as decimal, or `n/a` if the bit was not found (SPEC-006 FR-19).
 *
 * @param[in]  value_ns   Average to write.
 * @param[in]  high_ns    High average of the same bit; 0 means not found.
 * @param[out] text       Receives the null-terminated value; TUNER_RESULT_VALUE_TEXT_MAX bytes.
 */
static void FormatReadValue(uint32_t value_ns, uint32_t high_ns, char *text)
{
    if (high_ns == 0) {
        strcpy(text, "n/a");
    } else {
        snprintf(text, TUNER_RESULT_VALUE_TEXT_MAX, "%u", (unsigned)value_ns);
    }
}

size_t FormatTunerResult(tuner_result_state_t state, const ws2812_measurement_t *record, char *body,
                         size_t body_size)
{
    static const char *const s_state_names[] = {
        [TUNER_RESULT_PENDING] = "pending",
        [TUNER_RESULT_DONE] = "done",
        [TUNER_RESULT_TIMEOUT] = "timeout",
        [TUNER_RESULT_COUNT_ERROR] = "count_error",
        [TUNER_RESULT_NOT_MEASURED] = "not_measured",
        [TUNER_RESULT_SUPERSEDED] = "superseded",
        [TUNER_RESULT_UNKNOWN] = "unknown",
        [TUNER_RESULT_READ] = "read",
    };
    if (body == NULL || body_size == 0 || (unsigned)state >= sizeof(s_state_names) / sizeof(s_state_names[0])) {
        return 0;
    }

    int written;
    if (state == TUNER_RESULT_READ) {
        char bit0_high[TUNER_RESULT_VALUE_TEXT_MAX];
        char bit0_period[TUNER_RESULT_VALUE_TEXT_MAX];
        char bit1_high[TUNER_RESULT_VALUE_TEXT_MAX];
        char bit1_period[TUNER_RESULT_VALUE_TEXT_MAX];
        FormatReadValue(record->bit0_high_avg_ns, record->bit0_high_avg_ns, bit0_high);
        FormatReadValue(record->bit0_period_avg_ns, record->bit0_high_avg_ns, bit0_period);
        FormatReadValue(record->bit1_high_avg_ns, record->bit1_high_avg_ns, bit1_high);
        FormatReadValue(record->bit1_period_avg_ns, record->bit1_high_avg_ns, bit1_period);
        written = snprintf(body, body_size, "state=read&b0h=%s&b0p=%s&b1h=%s&b1p=%s", bit0_high, bit0_period,
                           bit1_high, bit1_period);
    } else if (state != TUNER_RESULT_DONE) {
        written = snprintf(body, body_size, "state=%s", s_state_names[state]);
    } else if (record->match_available) {
        written = snprintf(body, body_size, "state=done&b0=%u&b1=%u&match=%u", (unsigned)record->bit0_high_avg_ns,
                           (unsigned)record->bit1_high_avg_ns, (unsigned)record->match_count);
    } else {
        written = snprintf(body, body_size, "state=done&b0=%u&b1=%u&match=n/a", (unsigned)record->bit0_high_avg_ns,
                           (unsigned)record->bit1_high_avg_ns);
    }
    if (written < 0 || (size_t)written >= body_size) {
        body[0] = '\0';
        return 0;
    }
    return (size_t)written;
}
