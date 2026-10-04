/**
 * @file test_read_result.cpp
 * @brief SPEC-006 T-2 (FR-18, FR-19, FR-20, NFR-8): GetTunerResultState() and FormatTunerResult() for a READ_DONE
 *        record (section 7.3 bodies, `n/a`, the 70-byte maximum within TUNER_RESULT_BODY_MAX = 96), and regression of
 *        the SPEC-003 section 7.6 bodies and decision order.
 *
 * tuner_result.c has no ESP-IDF dependency and is linked alone.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <string>

extern "C" {
#include "http_portal.h"
#include "ws2812_timing.h"
}

namespace {

ws2812_measurement_t ReadRecord(uint32_t submit_seq, uint32_t b0h, uint32_t b0p, uint32_t b1h, uint32_t b1p)
{
    ws2812_measurement_t record = {};
    record.submit_seq = submit_seq;
    record.state = WS2812_MEASUREMENT_READ_DONE;
    record.bit0_high_avg_ns = b0h;
    record.bit0_period_avg_ns = b0p;
    record.bit1_high_avg_ns = b1h;
    record.bit1_period_avg_ns = b1p;
    return record;
}

ws2812_measurement_t Record(uint32_t submit_seq, ws2812_measurement_state_t state, uint32_t b0 = 0, uint32_t b1 = 0,
                            uint16_t match = 0, bool available = false)
{
    ws2812_measurement_t record = {};
    record.submit_seq = submit_seq;
    record.state = state;
    record.bit0_high_avg_ns = b0;
    record.bit1_high_avg_ns = b1;
    record.match_count = match;
    record.match_available = available;
    return record;
}

std::string Format(tuner_result_state_t state, const ws2812_measurement_t &record)
{
    char body[TUNER_RESULT_BODY_MAX];
    std::memset(body, 'x', sizeof(body));
    const size_t length = FormatTunerResult(state, &record, body, sizeof(body));
    REQUIRE(length == std::strlen(body));
    return body;
}

/** The body served for request @p seq with @p last issued, as the handler composes the two helpers. */
std::string Serve(uint32_t seq, uint32_t last, const ws2812_measurement_t &record)
{
    return Format(GetTunerResultState(seq, last, &record), record);
}

}  // namespace

// ---- FR-18: record layout ---------------------------------------------------------------------------------------------

TEST_CASE("the record gains two period fields and the READ_DONE state, at most 28 bytes", "[T-2][FR-18]")
{
    REQUIRE(sizeof(ws2812_measurement_t) <= 28);
    REQUIRE(sizeof(ws2812_measurement_t) == 28);
    REQUIRE(sizeof(((ws2812_measurement_t *)nullptr)->bit0_period_avg_ns) == 4);
    REQUIRE(sizeof(((ws2812_measurement_t *)nullptr)->bit1_period_avg_ns) == 4);
    // READ_DONE is a new value, distinct from every SPEC-003/SPEC-004 state (existing values unchanged).
    REQUIRE(WS2812_MEASUREMENT_DONE == 0);
    REQUIRE(WS2812_MEASUREMENT_TIMEOUT == 1);
    REQUIRE(WS2812_MEASUREMENT_COUNT_ERROR == 2);
    REQUIRE(WS2812_MEASUREMENT_NOT_MEASURED == 3);
    REQUIRE(WS2812_MEASUREMENT_READ_DONE == 4);
}

// ---- FR-19 / FR-20: state decision ----------------------------------------------------------------------------------

TEST_CASE("state: a matching READ_DONE record gives its own final state read", "[T-2][FR-19][FR-20]")
{
    const ws2812_measurement_t record = ReadRecord(5, 400, 1250, 800, 1250);
    REQUIRE(GetTunerResultState(5, 5, &record) == TUNER_RESULT_READ);
    REQUIRE(GetTunerResultState(5, 9, &record) == TUNER_RESULT_READ);
}

TEST_CASE("state: the SPEC-003 decision order is unchanged around a READ_DONE record", "[T-2][FR-19]")
{
    const ws2812_measurement_t record = ReadRecord(5, 400, 1250, 800, 1250);
    REQUIRE(GetTunerResultState(0, 9, &record) == TUNER_RESULT_UNKNOWN);       // 0 first
    REQUIRE(GetTunerResultState(10, 9, &record) == TUNER_RESULT_UNKNOWN);      // not issued
    REQUIRE(GetTunerResultState(6, 5, &record) == TUNER_RESULT_UNKNOWN);       // unknown before the record check
    REQUIRE(GetTunerResultState(4, 9, &record) == TUNER_RESULT_SUPERSEDED);    // record holds a newer number
    REQUIRE(GetTunerResultState(6, 9, &record) == TUNER_RESULT_PENDING);       // record older
}

TEST_CASE("state: the existing record states still map as in SPEC-003 (regression)", "[T-2][FR-19]")
{
    const ws2812_measurement_t done = Record(3, WS2812_MEASUREMENT_DONE);
    REQUIRE(GetTunerResultState(3, 3, &done) == TUNER_RESULT_DONE);
    const ws2812_measurement_t timeout = Record(3, WS2812_MEASUREMENT_TIMEOUT);
    const ws2812_measurement_t count_error = Record(3, WS2812_MEASUREMENT_COUNT_ERROR);
    const ws2812_measurement_t not_measured = Record(3, WS2812_MEASUREMENT_NOT_MEASURED);
    const ws2812_measurement_t never_set = {};
    REQUIRE(GetTunerResultState(3, 3, &timeout) == TUNER_RESULT_TIMEOUT);
    REQUIRE(GetTunerResultState(3, 3, &count_error) == TUNER_RESULT_COUNT_ERROR);
    REQUIRE(GetTunerResultState(3, 3, &not_measured) == TUNER_RESULT_NOT_MEASURED);
    REQUIRE(GetTunerResultState(1, 3, &never_set) == TUNER_RESULT_PENDING);
}

// ---- FR-19: section 7.3 bodies ----------------------------------------------------------------------------------------

TEST_CASE("format: section 7.3 READ_DONE, both bits found", "[T-2][FR-19]")
{
    REQUIRE(Serve(7, 7, ReadRecord(7, 400, 1250, 800, 1250)) == "state=read&b0h=400&b0p=1250&b1h=800&b1p=1250");
}

TEST_CASE("format: section 7.3 READ_DONE, bit 0 not found -> both bit-0 fields n/a", "[T-2][FR-19]")
{
    REQUIRE(Serve(7, 7, ReadRecord(7, 0, 0, 800, 1250)) == "state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250");
}

TEST_CASE("format: READ_DONE, bit 1 not found -> both bit-1 fields n/a", "[T-2][FR-19]")
{
    REQUIRE(Serve(7, 7, ReadRecord(7, 400, 1250, 0, 0)) == "state=read&b0h=400&b0p=1250&b1h=n/a&b1p=n/a");
}

TEST_CASE("format: n/a is decided by the high average: a zero high hides a non-zero period", "[T-2][FR-19]")
{
    // FR-19: "both fields of a class are n/a when its high average is 0".
    REQUIRE(Format(TUNER_RESULT_READ, ReadRecord(1, 0, 1250, 800, 1250)) ==
            "state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250");
}

TEST_CASE("format: section 7.3 read failures reuse the SPEC-003 bodies", "[T-2][FR-19]")
{
    REQUIRE(Serve(8, 8, Record(8, WS2812_MEASUREMENT_TIMEOUT)) == "state=timeout");
    REQUIRE(Serve(8, 8, Record(8, WS2812_MEASUREMENT_COUNT_ERROR)) == "state=count_error");
    REQUIRE(Serve(8, 8, Record(8, WS2812_MEASUREMENT_NOT_MEASURED)) == "state=not_measured");
}

TEST_CASE("format: the longest read body is 70 bytes and fits within TUNER_RESULT_BODY_MAX = 96", "[T-2][FR-19]")
{
    const uint32_t max = 4294967295u;
    const std::string longest = Format(TUNER_RESULT_READ, ReadRecord(1, max, max, max, max));
    REQUIRE(longest == "state=read&b0h=4294967295&b0p=4294967295&b1h=4294967295&b1p=4294967295");
    REQUIRE(longest.size() == 70);
    REQUIRE(TUNER_RESULT_BODY_MAX == 96);
    REQUIRE(longest.size() < TUNER_RESULT_BODY_MAX);

    char exact[71];
    const ws2812_measurement_t record = ReadRecord(1, max, max, max, max);
    REQUIRE(FormatTunerResult(TUNER_RESULT_READ, &record, exact, sizeof(exact)) == 70);
    char short_by_one[70];
    std::memset(short_by_one, 'x', sizeof(short_by_one));
    REQUIRE(FormatTunerResult(TUNER_RESULT_READ, &record, short_by_one, sizeof(short_by_one)) == 0);
    REQUIRE(short_by_one[0] == '\0');
}

TEST_CASE("format: the read body is URLSearchParams key=value pairs, no duty value", "[T-2][FR-19][NFR-10]")
{
    const std::string body = Format(TUNER_RESULT_READ, ReadRecord(1, 401, 1251, 799, 1249));
    REQUIRE(body == "state=read&b0h=401&b0p=1251&b1h=799&b1p=1249");
    REQUIRE(body.find(' ') == std::string::npos);
    REQUIRE(body.find('\n') == std::string::npos);
    REQUIRE(body.find("duty") == std::string::npos);
    REQUIRE(body.find("match") == std::string::npos);
}

TEST_CASE("format: a READ_DONE record served for another number never leaks the read values", "[T-2][FR-19][FR-20]")
{
    const ws2812_measurement_t record = ReadRecord(5, 400, 1250, 800, 1250);
    REQUIRE(Serve(4, 9, record) == "state=superseded");
    REQUIRE(Serve(6, 9, record) == "state=pending");
    REQUIRE(Serve(0, 9, record) == "state=unknown");
}

// ---- Regression: SPEC-003 section 7.6 bodies unchanged ---------------------------------------------------------------

TEST_CASE("regression: the SPEC-003 section 7.6 bodies are unchanged", "[T-2][FR-19][SPEC-003]")
{
    REQUIRE(Format(TUNER_RESULT_DONE, Record(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true)) ==
            "state=done&b0=400&b1=800&match=144");
    REQUIRE(Format(TUNER_RESULT_DONE, Record(4, WS2812_MEASUREMENT_DONE, 500, 500, 0, false)) ==
            "state=done&b0=500&b1=500&match=n/a");
    const ws2812_measurement_t any = Record(1, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    REQUIRE(Format(TUNER_RESULT_PENDING, any) == "state=pending");
    REQUIRE(Format(TUNER_RESULT_TIMEOUT, any) == "state=timeout");
    REQUIRE(Format(TUNER_RESULT_COUNT_ERROR, any) == "state=count_error");
    REQUIRE(Format(TUNER_RESULT_NOT_MEASURED, any) == "state=not_measured");
    REQUIRE(Format(TUNER_RESULT_SUPERSEDED, any) == "state=superseded");
    REQUIRE(Format(TUNER_RESULT_UNKNOWN, any) == "state=unknown");
    REQUIRE(Format(TUNER_RESULT_DONE, Record(1, WS2812_MEASUREMENT_DONE, 4294967295u, 4294967295u, 144, true)).size() ==
            48);
}

TEST_CASE("regression: a DONE record with non-zero period fields still formats the SPEC-003 body", "[T-2][FR-18][FR-19]")
{
    ws2812_measurement_t record = Record(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    record.bit0_period_avg_ns = 1250;   // never set for DONE (FR-18), and never shown if it were
    record.bit1_period_avg_ns = 1250;
    REQUIRE(Format(TUNER_RESULT_DONE, record) == "state=done&b0=400&b1=800&match=144");
}

TEST_CASE("format: an invalid state beyond TUNER_RESULT_READ yields 0", "[T-2][FR-19]")
{
    const ws2812_measurement_t record = ReadRecord(1, 400, 1250, 800, 1250);
    char body[TUNER_RESULT_BODY_MAX];
    REQUIRE(FormatTunerResult(static_cast<tuner_result_state_t>(TUNER_RESULT_READ + 1), &record, body, sizeof(body)) ==
            0);
}
