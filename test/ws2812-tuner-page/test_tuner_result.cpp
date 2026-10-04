/**
 * @file test_tuner_result.cpp
 * @brief SPEC-003 T-17 (pure part): the GET /tuner/result helpers of main/http_portal/tuner_result.c
 *        (FR-25 ParseTunerResultSeq(), FR-26 GetTunerResultState(), section 7.6 FormatTunerResult(), NFR-4).
 *
 * tuner_result.c has no ESP-IDF dependency, so it is linked alone. The handler around these helpers (query buffer
 * length limit, mutex, headers, logging) is tested through the httpd simulator in test_tuner_http.cpp.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "http_portal.h"
#include "ws2812_timing.h"
}

namespace {

/** Parse @p query; returns the seq or -1 when malformed. */
int64_t Parse(const char *query)
{
    uint32_t seq = 0xA5A5A5A5u;
    return ParseTunerResultSeq(query, &seq) ? static_cast<int64_t>(seq) : -1;
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

}  // namespace

// ---- FR-25: ParseTunerResultSeq() ---------------------------------------------------------------------------------

TEST_CASE("ParseTunerResultSeq accepts 1 to 10 digits up to 4,294,967,295", "[T-17][FR-25]")
{
    REQUIRE(Parse("seq=1") == 1);
    REQUIRE(Parse("seq=3") == 3);
    REQUIRE(Parse("seq=0") == 0);                       // well-formed; the state function maps it to unknown
    REQUIRE(Parse("seq=99") == 99);
    REQUIRE(Parse("seq=4294967295") == 4294967295LL);   // 10 digits, UINT32_MAX
    REQUIRE(Parse("seq=0000000001") == 1);              // 10 digits with leading zeros
    REQUIRE(Parse("seq=1234567890") == 1234567890LL);
}

TEST_CASE("ParseTunerResultSeq ignores other keys and uses the first seq", "[T-17][FR-25]")
{
    REQUIRE(Parse("x=1&seq=5") == 5);
    REQUIRE(Parse("seq=5&x=1") == 5);
    REQUIRE(Parse("&seq=6") == 6);
    REQUIRE(Parse("seq=7&") == 7);
    REQUIRE(Parse("xseq=1&seq=8") == 8);                // "xseq" is another key
    REQUIRE(Parse("seqx=1&seq=9") == 9);                // "seqx" is another key
    // Duplicate keys: FR-25 takes the value from the first occurrence, so the first one alone decides.
    REQUIRE(Parse("seq=7&seq=9") == 7);
    REQUIRE(Parse("seq=7&seq=abc") == 7);
    REQUIRE(Parse("seq=abc&seq=7") == -1);
    REQUIRE(Parse("seq=&seq=7") == -1);
}

TEST_CASE("ParseTunerResultSeq rejects a missing, empty or valueless seq", "[T-17][FR-25]")
{
    REQUIRE(Parse(nullptr) == -1);
    REQUIRE(Parse("") == -1);                           // empty query
    REQUIRE(Parse("x=1") == -1);                        // missing
    REQUIRE(Parse("seq") == -1);                        // bare key, no '='
    REQUIRE(Parse("seq&x=1") == -1);
    REQUIRE(Parse("seq=") == -1);                       // empty value
    REQUIRE(Parse("seqx=3") == -1);                     // not the key "seq"
    REQUIRE(Parse("xseq=3") == -1);
    REQUIRE(Parse("SEQ=3") == -1);                      // keys are case-sensitive
    uint32_t seq = 0;
    REQUIRE_FALSE(ParseTunerResultSeq("seq=1", nullptr));
    REQUIRE_FALSE(ParseTunerResultSeq(nullptr, &seq));
}

TEST_CASE("ParseTunerResultSeq rejects non-digits, signs, spaces and encodings", "[T-17][FR-25][NFR-11]")
{
    for (const char *query : {"seq=abc", "seq=1a", "seq=a1", "seq=-1", "seq=+1", "seq= 1", "seq=1 ", "seq=%31",
                              "seq=1.0", "seq=0x10", "seq=1e3", "seq=<script>"}) {
        INFO(query);
        REQUIRE(Parse(query) == -1);
    }
}

TEST_CASE("ParseTunerResultSeq rejects overflow and more than 10 digits", "[T-17][FR-25]")
{
    REQUIRE(Parse("seq=4294967296") == -1);             // UINT32_MAX + 1
    REQUIRE(Parse("seq=9999999999") == -1);             // 10 digits above UINT32_MAX
    REQUIRE(Parse("seq=12345678901") == -1);            // 11 digits
    REQUIRE(Parse("seq=00000000001") == -1);            // 11 digits even with leading zeros
    REQUIRE(Parse("seq=18446744073709551617") == -1);   // would wrap a 64-bit accumulator
}

TEST_CASE("ParseTunerResultSeq leaves the output untouched on failure", "[T-17][FR-25]")
{
    uint32_t seq = 42;
    REQUIRE_FALSE(ParseTunerResultSeq("seq=4294967296", &seq));
    REQUIRE(seq == 42);
}

// ---- FR-26: GetTunerResultState() decision table -------------------------------------------------------------------

TEST_CASE("state: unknown for 0 and for a number not issued yet (checked first)", "[T-17][FR-26]")
{
    const ws2812_measurement_t record = Record(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    REQUIRE(GetTunerResultState(0, 3, &record) == TUNER_RESULT_UNKNOWN);
    REQUIRE(GetTunerResultState(4, 3, &record) == TUNER_RESULT_UNKNOWN);
    REQUIRE(GetTunerResultState(99, 3, &record) == TUNER_RESULT_UNKNOWN);
    REQUIRE(GetTunerResultState(4294967295u, 3, &record) == TUNER_RESULT_UNKNOWN);
    // After a reboot nothing is issued: every number is unknown, even one the record would match.
    const ws2812_measurement_t empty = Record(0, WS2812_MEASUREMENT_DONE);
    REQUIRE(GetTunerResultState(1, 0, &empty) == TUNER_RESULT_UNKNOWN);
    REQUIRE(GetTunerResultState(0, 0, &empty) == TUNER_RESULT_UNKNOWN);
    // A record with submit_seq 0 (boot frame) never answers seq 0.
    REQUIRE(GetTunerResultState(0, 5, &empty) == TUNER_RESULT_UNKNOWN);
}

TEST_CASE("state: the record's own state when its submit_seq equals the request", "[T-17][FR-26]")
{
    const struct { ws2812_measurement_state_t stored; tuner_result_state_t served; } kCases[] = {
        {WS2812_MEASUREMENT_DONE, TUNER_RESULT_DONE},
        {WS2812_MEASUREMENT_TIMEOUT, TUNER_RESULT_TIMEOUT},
        {WS2812_MEASUREMENT_COUNT_ERROR, TUNER_RESULT_COUNT_ERROR},
        {WS2812_MEASUREMENT_NOT_MEASURED, TUNER_RESULT_NOT_MEASURED},
    };
    for (const auto &test_case : kCases) {
        const ws2812_measurement_t record = Record(5, test_case.stored);
        REQUIRE(GetTunerResultState(5, 5, &record) == test_case.served);
        REQUIRE(GetTunerResultState(5, 9, &record) == test_case.served);   // later numbers issued meanwhile
    }
}

TEST_CASE("state: superseded when the record holds a newer number, pending when older", "[T-17][FR-26]")
{
    const ws2812_measurement_t record = Record(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    REQUIRE(GetTunerResultState(2, 5, &record) == TUNER_RESULT_SUPERSEDED);
    REQUIRE(GetTunerResultState(1, 5, &record) == TUNER_RESULT_SUPERSEDED);
    REQUIRE(GetTunerResultState(4, 5, &record) == TUNER_RESULT_PENDING);
    REQUIRE(GetTunerResultState(5, 5, &record) == TUNER_RESULT_PENDING);
    // Superseded also for a non-done newer outcome.
    const ws2812_measurement_t newer_not_measured = Record(6, WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(GetTunerResultState(5, 6, &newer_not_measured) == TUNER_RESULT_SUPERSEDED);
}

TEST_CASE("state: a never-set record (all zero) gives pending for every issued number", "[T-17][FR-24][FR-26]")
{
    const ws2812_measurement_t never_set = {};
    REQUIRE(GetTunerResultState(1, 1, &never_set) == TUNER_RESULT_PENDING);
    REQUIRE(GetTunerResultState(7, 9, &never_set) == TUNER_RESULT_PENDING);
    REQUIRE(GetTunerResultState(10, 9, &never_set) == TUNER_RESULT_UNKNOWN);
}

TEST_CASE("state: the boundary between pending and unknown is the last issued number", "[T-17][FR-26]")
{
    const ws2812_measurement_t never_set = {};
    REQUIRE(GetTunerResultState(4294967295u, 4294967295u, &never_set) == TUNER_RESULT_PENDING);
    REQUIRE(GetTunerResultState(8, 8, &never_set) == TUNER_RESULT_PENDING);
    REQUIRE(GetTunerResultState(9, 8, &never_set) == TUNER_RESULT_UNKNOWN);
}

// ---- Section 7.6: FormatTunerResult() exact bodies -----------------------------------------------------------------

TEST_CASE("format: the section 7.6 table bodies", "[T-17][FR-25][FR-26]")
{
    REQUIRE(Format(TUNER_RESULT_DONE, Record(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true)) ==
            "state=done&b0=400&b1=800&match=144");
    REQUIRE(Format(TUNER_RESULT_DONE, Record(4, WS2812_MEASUREMENT_DONE, 500, 500, 0, false)) ==
            "state=done&b0=500&b1=500&match=n/a");   // vector AC
    REQUIRE(Format(TUNER_RESULT_DONE, Record(4, WS2812_MEASUREMENT_DONE, 500, 500, 77, false)) ==
            "state=done&b0=500&b1=500&match=n/a");   // a count is never shown when not available
    REQUIRE(Format(TUNER_RESULT_DONE, Record(4, WS2812_MEASUREMENT_DONE, 600, 500, 0, true)) ==
            "state=done&b0=600&b1=500&match=0");
    const ws2812_measurement_t any = Record(1, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    REQUIRE(Format(TUNER_RESULT_PENDING, any) == "state=pending");
    REQUIRE(Format(TUNER_RESULT_TIMEOUT, any) == "state=timeout");
    REQUIRE(Format(TUNER_RESULT_COUNT_ERROR, any) == "state=count_error");
    REQUIRE(Format(TUNER_RESULT_NOT_MEASURED, any) == "state=not_measured");
    REQUIRE(Format(TUNER_RESULT_SUPERSEDED, any) == "state=superseded");
    REQUIRE(Format(TUNER_RESULT_UNKNOWN, any) == "state=unknown");
}

TEST_CASE("format: the longest body is 48 bytes and fits TUNER_RESULT_BODY_MAX", "[T-17][NFR-4]")
{
    const std::string longest =
        Format(TUNER_RESULT_DONE, Record(1, WS2812_MEASUREMENT_DONE, 4294967295u, 4294967295u, 144, true));
    REQUIRE(longest == "state=done&b0=4294967295&b1=4294967295&match=144");
    REQUIRE(longest.size() == 48);
    REQUIRE(longest.size() < TUNER_RESULT_BODY_MAX);
    REQUIRE(TUNER_RESULT_BODY_MAX == 96);   // SPEC-006 FR-19 (2026-10-04): was 64
    REQUIRE(TUNER_RESULT_QUERY_MAX == 32);
}

TEST_CASE("format: bodies are URLSearchParams key=value pairs with no client text", "[T-17][FR-25][NFR-11]")
{
    const std::string body = Format(TUNER_RESULT_DONE, Record(2, WS2812_MEASUREMENT_DONE, 1, 2, 3, true));
    REQUIRE(body == "state=done&b0=1&b1=2&match=3");
    REQUIRE(body.find(' ') == std::string::npos);
    REQUIRE(body.find('\n') == std::string::npos);
}

TEST_CASE("format: a too-small buffer or an invalid state yields 0 and an empty body", "[T-17][NFR-4]")
{
    const ws2812_measurement_t record = Record(1, WS2812_MEASUREMENT_DONE, 4294967295u, 4294967295u, 144, true);
    char body[48];   // one byte short of the 48-character body plus terminator
    std::memset(body, 'x', sizeof(body));
    REQUIRE(FormatTunerResult(TUNER_RESULT_DONE, &record, body, sizeof(body)) == 0);
    REQUIRE(body[0] == '\0');
    char exact[49];
    REQUIRE(FormatTunerResult(TUNER_RESULT_DONE, &record, exact, sizeof(exact)) == 48);
    REQUIRE(FormatTunerResult(static_cast<tuner_result_state_t>(99), &record, exact, sizeof(exact)) == 0);
    REQUIRE(FormatTunerResult(TUNER_RESULT_PENDING, &record, nullptr, 10) == 0);
    REQUIRE(FormatTunerResult(TUNER_RESULT_PENDING, &record, exact, 0) == 0);
}
