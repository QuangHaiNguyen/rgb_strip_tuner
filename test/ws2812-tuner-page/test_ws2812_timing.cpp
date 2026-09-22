/**
 * @file test_ws2812_timing.cpp
 * @brief Host tests for ParseTunerForm() and ValidateWs2812Timing() (SPEC-003 T-1, T-3;
 *        FR-15, FR-16, NFR-9, NFR-15; section 7.3 rules V1-V3; section 7.5 vectors A-V).
 *
 * Vector S (body-length cap, 96 vs 97 bytes) is FR-15's `content_len` check, which lives in
 * HandleTunerSubmitRequest(), not in ParseTunerForm(); it is exercised at the HTTP layer in
 * test_tuner_http.cpp instead.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "ws2812_timing.h"
}

namespace {

struct ExpectedTiming {
    uint16_t bit0_high_ns, bit0_period_ns, bit1_high_ns, bit1_period_ns, reset_us;
};

bool operator==(const ws2812_timing_t &a, const ExpectedTiming &b)
{
    return a.bit0_high_ns == b.bit0_high_ns && a.bit0_period_ns == b.bit0_period_ns &&
           a.bit1_high_ns == b.bit1_high_ns && a.bit1_period_ns == b.bit1_period_ns &&
           a.reset_us == b.reset_us;
}

/** Parse @p body and return the validation result; REQUIREs the parse itself succeeds. */
ws2812_timing_result_t ParseAndValidate(const char *body, ws2812_timing_t *timing = nullptr)
{
    ws2812_timing_t local = {};
    ws2812_timing_t *target = timing != nullptr ? timing : &local;
    REQUIRE(ParseTunerForm(body, target));
    return ValidateWs2812Timing(target);
}

/** Default valid timing set (vector A / section 7.1 defaults). */
ws2812_timing_t Defaults()
{
    return {TUNER_DEFAULT_BIT0_HIGH_NS, TUNER_DEFAULT_BIT0_PERIOD_NS, TUNER_DEFAULT_BIT1_HIGH_NS,
            TUNER_DEFAULT_BIT1_PERIOD_NS, TUNER_DEFAULT_RESET_US};
}

}  // namespace

// ---- Vectors A-E, T, U: accepted requests -----------------------------------------------------------------------

TEST_CASE("vector A: datasheet defaults are accepted", "[T-1][FR-15][FR-16]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
    REQUIRE(timing == ExpectedTiming{400, 1250, 800, 1250, 280});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

TEST_CASE("vector B: minimum edge with equal bit0/bit1 highs is accepted", "[T-1][T-3][FR-15][FR-16]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", &timing));
    REQUIRE(timing == ExpectedTiming{100, 800, 100, 800, 50});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);   // no rule relates bit0/bit1 highs
}

TEST_CASE("vector C: low-time edge, exactly 100 ns, is accepted", "[T-1][T-3][FR-15][FR-16]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", &timing));
    REQUIRE(timing == ExpectedTiming{1075, 1200, 1100, 1200, 280});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);   // bit1 low time = 1200-1100 = 100
}

TEST_CASE("vector D: maximum edge with inverted highs is accepted", "[T-1][T-3][FR-15][FR-16]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800", &timing));
    REQUIRE(timing == ExpectedTiming{1200, 2000, 1000, 2000, 800});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);   // bit1 high < bit0 high: allowed
}

TEST_CASE("vector E: leading zeros parse to the same values as vector A", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", &timing));
    REQUIRE(timing == ExpectedTiming{400, 1250, 800, 1250, 280});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

TEST_CASE("vector T: a duplicate key uses the first occurrence", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
    REQUIRE(timing == ExpectedTiming{400, 1250, 800, 1250, 280});   // the later, invalid 90 is ignored
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

TEST_CASE("vector U: an extra key is ignored", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280&x=1", &timing));
    REQUIRE(timing == ExpectedTiming{400, 1250, 800, 1250, 280});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

// ---- Vectors F-M, V: parsed but rejected by ValidateWs2812Timing() -----------------------------------------------

TEST_CASE("vector F: bit0 high below the minimum is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector G: bit0 high not a multiple of the step is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector H: bit0 period above the maximum is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector I: reset time below the minimum is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector J: reset time not a multiple of its step is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector K: reset time above the maximum is out_of_range", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("vector L: bit1 low time of 75 ns is bad_combination", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280") ==
            WS2812_TIMING_BAD_COMBINATION);
}

TEST_CASE("vector M: bit0 low time of 0 ns is bad_combination", "[T-1][T-3][FR-16]")
{
    REQUIRE(ParseAndValidate("b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280") ==
            WS2812_TIMING_BAD_COMBINATION);
}

TEST_CASE("vector V: a range fault and a combination fault together report out_of_range", "[T-1][T-3][FR-16]")
{
    // b0h_ns=90 fails V1; b1h_ns=1100/b1p_ns=1175 fails V3. The range check runs first (section 7.3).
    REQUIRE(ParseAndValidate("b0h_ns=90&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280") ==
            WS2812_TIMING_OUT_OF_RANGE);
}

// ---- Vectors N-R: malformed bodies, rejected by ParseTunerForm() itself ------------------------------------------

TEST_CASE("vector N: a missing key is malformed", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250", &timing));
}

TEST_CASE("vector O: a non-numeric value is malformed", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

TEST_CASE("vector P: an empty value is malformed", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

TEST_CASE("vector Q: a signed value is malformed", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

TEST_CASE("vector R: a 5-digit value is malformed", "[T-1][FR-15][NFR-9]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

// ---- FR-15 digit-rule and key-lookup detail, beyond the lettered vectors ------------------------------------------

TEST_CASE("1 to 4 digit values are accepted; 0 and 5+ digits are not", "[T-1][FR-15][NFR-9]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=1&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", &timing));       // 1 digit
    REQUIRE(timing.bit0_high_ns == 1);
    REQUIRE(ParseTunerForm("b0h_ns=1200&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800", &timing)); // 4 digits
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));       // 0 digits
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=99999&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));  // 5 digits
}

TEST_CASE("percent-encoded and whitespace values are non-numeric and malformed", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=%34%30%30&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=4 0&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
    REQUIRE_FALSE(ParseTunerForm("b0h_ns=+400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

TEST_CASE("each of the five keys is individually required", "[T-1][FR-15]")
{
    const char *const kBodies[] = {
        "b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",             // b0h_ns missing
        "b0h_ns=400&b1h_ns=800&b1p_ns=1250&rst_us=280",              // b0p_ns missing
        "b0h_ns=400&b0p_ns=1250&b1p_ns=1250&rst_us=280",             // b1h_ns missing
        "b0h_ns=400&b0p_ns=1250&b1h_ns=800&rst_us=280",              // b1p_ns missing
        "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250",             // rst_us missing
        "",                                                          // empty body
    };
    for (const char *body : kBodies) {
        INFO(body);
        ws2812_timing_t timing = {};
        REQUIRE_FALSE(ParseTunerForm(body, &timing));
    }
}

TEST_CASE("a key is matched by exact name, not as a prefix or suffix", "[T-1][FR-15]")
{
    ws2812_timing_t timing = {};
    // "xb0h_ns" is not "b0h_ns", and "b0h_nsx=..." does not match either; both leave b0h_ns missing.
    REQUIRE_FALSE(ParseTunerForm("xb0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
    REQUIRE_FALSE(ParseTunerForm("b0h_nsx=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", &timing));
}

// ---- T-3: boundary sweep of ValidateWs2812Timing() (section 7.3) -------------------------------------------------

TEST_CASE("bit0 high time: Min-25, Min, Max, Max+25 and a non-multiple of 25", "[T-3][FR-16]")
{
    ws2812_timing_t timing = Defaults();
    timing.bit0_high_ns = TUNER_HIGH_MIN_NS - TUNER_STEP_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
    timing.bit0_high_ns = TUNER_HIGH_MIN_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit0_high_ns = TUNER_HIGH_MAX_NS;
    timing.bit0_period_ns = TUNER_PERIOD_MAX_NS;   // keep low time >= 100 ns at the high edge
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit0_high_ns = TUNER_HIGH_MAX_NS + TUNER_STEP_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
    timing.bit0_high_ns = TUNER_DEFAULT_BIT0_HIGH_NS + 10;   // not a multiple of 25
    timing.bit0_period_ns = TUNER_DEFAULT_BIT0_PERIOD_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("bit1 period: Min-25, Min, Max, Max+25 and a non-multiple of 25", "[T-3][FR-16]")
{
    ws2812_timing_t timing = Defaults();
    timing.bit1_period_ns = TUNER_PERIOD_MIN_NS - TUNER_STEP_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
    timing.bit1_period_ns = TUNER_PERIOD_MIN_NS;
    timing.bit1_high_ns = TUNER_HIGH_MIN_NS;   // low time = 700 ns, keep V3 satisfied
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit1_period_ns = TUNER_PERIOD_MAX_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit1_period_ns = TUNER_PERIOD_MAX_NS + TUNER_STEP_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
    timing.bit1_period_ns = TUNER_DEFAULT_BIT1_PERIOD_NS + 15;   // not a multiple of 25
    timing.bit1_high_ns = TUNER_DEFAULT_BIT1_HIGH_NS;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("reset time: 40, 50, 60, 285, 800, 810 us", "[T-3][FR-16]")
{
    ws2812_timing_t timing = Defaults();
    const struct { uint16_t reset_us; ws2812_timing_result_t expected; } kCases[] = {
        {40, WS2812_TIMING_OUT_OF_RANGE},    // below minimum
        {50, WS2812_TIMING_OK},              // minimum
        {60, WS2812_TIMING_OK},              // on-step, mid-range
        {285, WS2812_TIMING_OUT_OF_RANGE},   // not a multiple of 10
        {800, WS2812_TIMING_OK},             // maximum
        {810, WS2812_TIMING_OUT_OF_RANGE},   // above maximum
    };
    for (const auto &test_case : kCases) {
        INFO(test_case.reset_us);
        timing.reset_us = test_case.reset_us;
        REQUIRE(ValidateWs2812Timing(&timing) == test_case.expected);
    }
}

TEST_CASE("V3 low-time boundary at 75, 100, 125 ns for bit0", "[T-3][FR-16]")
{
    ws2812_timing_t timing = Defaults();
    timing.bit0_period_ns = 1250;
    timing.bit0_high_ns = 1250 - 75;    // low time 75 ns: violates V3
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_BAD_COMBINATION);
    timing.bit0_high_ns = 1250 - 100;   // low time 100 ns: exactly the minimum, accepted
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit0_high_ns = 1250 - 125;   // low time 125 ns: comfortably accepted
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

TEST_CASE("V3 low-time boundary at 75, 100, 125 ns for bit1", "[T-3][FR-16]")
{
    ws2812_timing_t timing = Defaults();
    timing.bit1_period_ns = 1250;
    timing.bit1_high_ns = 1250 - 75;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_BAD_COMBINATION);
    timing.bit1_high_ns = 1250 - 100;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit1_high_ns = 1250 - 125;
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
}

TEST_CASE("equal and inverted bit0/bit1 high times are both accepted", "[T-1][T-3][FR-16]")
{
    ws2812_timing_t equal = Defaults();
    equal.bit1_high_ns = equal.bit0_high_ns;   // equal highs (also covered end-to-end by vector B)
    REQUIRE(ValidateWs2812Timing(&equal) == WS2812_TIMING_OK);

    ws2812_timing_t inverted = Defaults();
    uint16_t swap = inverted.bit0_high_ns;
    inverted.bit0_high_ns = inverted.bit1_high_ns;   // bit1 > bit0 becomes bit0 > bit1 (also vector D)
    inverted.bit1_high_ns = swap;
    REQUIRE(ValidateWs2812Timing(&inverted) == WS2812_TIMING_OK);
}
