/**
 * @file test_ws2812_timing.cpp
 * @brief Host tests for ParseTunerForm() and ValidateWs2812Timing() (SPEC-003 T-1, T-3, T-15;
 *        FR-15, FR-16, NFR-9, NFR-15; section 7.3 rules V1-V4; section 7.5 vectors A-AF).
 *
 * Vector S (body-length cap, 96 vs 97 bytes) is FR-15's `content_len` check, which lives in
 * HandleTunerSubmitRequest(), not in ParseTunerForm(); it is exercised at the HTTP layer in
 * test_tuner_http.cpp instead.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("vector B: minimum edge with equal highs and periods is bad_duty_order (V4)", "[T-1][T-3][T-15][FR-15][FR-16]")
{
    // Changed 2026-10-03: equal duty (80,000 >= 80,000) is an error under V4; was accepted before.
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", &timing));
    REQUIRE(timing == ExpectedTiming{100, 800, 100, 800, 50});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_BAD_DUTY_ORDER);
}

TEST_CASE("vector C: low-time edge, exactly 100 ns, is accepted", "[T-1][T-3][FR-15][FR-16]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", &timing));
    REQUIRE(timing == ExpectedTiming{1075, 1200, 1100, 1200, 280});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);   // bit1 low time = 1200-1100 = 100
}

TEST_CASE("vector D: maximum edge with inverted highs and equal periods is bad_duty_order (V4)", "[T-1][T-3][T-15][FR-15][FR-16]")
{
    // Changed 2026-10-03: 2,400,000 >= 2,000,000 fails V4; was accepted before.
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800", &timing));
    REQUIRE(timing == ExpectedTiming{1200, 2000, 1000, 2000, 800});
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_BAD_DUTY_ORDER);
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
    // Changed 2026-10-03: bit-1 high 700 (was 100) keeps V3 (low 100 ns) and V4 (320,000 < 875,000 at period 800,
    // 800,000 < 875,000 at period 2,000) satisfied, so only the period's own range/step decides.
    timing.bit1_high_ns = 700;
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
    // Changed 2026-10-03: with the defaults' bit 1 (800/1250) a bit-0 high of 1,150/1,250 fails V4. Bit 1 is set to
    // 1,200/1,300 (the highest duty V3 allows, 92.3 %) and bit 0 to period 1,200, so V4 holds at low 100 and 125 ns
    // (1,430,000 < 1,440,000 and 1,397,500 < 1,440,000) and only V3 decides.
    ws2812_timing_t timing = Defaults();
    timing.bit1_high_ns = 1200;
    timing.bit1_period_ns = 1300;
    timing.bit0_period_ns = 1200;
    timing.bit0_high_ns = 1200 - 75;    // low time 75 ns: violates V3
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_BAD_COMBINATION);
    timing.bit0_high_ns = 1200 - 100;   // low time 100 ns: exactly the minimum, accepted
    REQUIRE(ValidateWs2812Timing(&timing) == WS2812_TIMING_OK);
    timing.bit0_high_ns = 1200 - 125;   // low time 125 ns: comfortably accepted
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

TEST_CASE("equal or inverted high times: rejected with equal periods, accepted with a shorter bit-1 period", "[T-1][T-3][T-15][FR-16]")
{
    // Changed 2026-10-03: V4 orders the duties, not the high times (section 7.3). With equal periods equal or
    // inverted highs mean equal or inverted duty and are rejected; was "both accepted" before.
    ws2812_timing_t equal = Defaults();
    equal.bit1_high_ns = equal.bit0_high_ns;
    REQUIRE(ValidateWs2812Timing(&equal) == WS2812_TIMING_BAD_DUTY_ORDER);

    ws2812_timing_t inverted = Defaults();
    uint16_t swap = inverted.bit0_high_ns;
    inverted.bit0_high_ns = inverted.bit1_high_ns;
    inverted.bit1_high_ns = swap;
    REQUIRE(ValidateWs2812Timing(&inverted) == WS2812_TIMING_BAD_DUTY_ORDER);

    const ws2812_timing_t equal_highs_shorter_period = {500, 1250, 500, 1000, 280};   // vector AC
    REQUIRE(ValidateWs2812Timing(&equal_highs_shorter_period) == WS2812_TIMING_OK);
    const ws2812_timing_t inverted_highs_shorter_period = {600, 2000, 500, 1000, 280};   // vector AD
    REQUIRE(ValidateWs2812Timing(&inverted_highs_shorter_period) == WS2812_TIMING_OK);
}

// ---- T-1 / T-15 (2026-10-03): all 32 reference vectors A to AF through ParseTunerForm() + ValidateWs2812Timing() ----

namespace {

enum class Expect { kOk, kOutOfRange, kBadCombination, kBadDutyOrder, kMalformed };

struct ReferenceVector {
    const char *id;
    std::string body;
    Expect expected;
};

std::vector<ReferenceVector> AllVectors()
{
    const std::string a = "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280";
    return {
        {"A", a, Expect::kOk},
        {"B", "b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", Expect::kBadDutyOrder},
        {"C", "b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", Expect::kOk},
        {"D", "b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800", Expect::kBadDutyOrder},
        {"E", "b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", Expect::kOk},
        {"F", "b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kOutOfRange},
        {"G", "b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kOutOfRange},
        {"H", "b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kOutOfRange},
        {"I", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40", Expect::kOutOfRange},
        {"J", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285", Expect::kOutOfRange},
        {"K", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810", Expect::kOutOfRange},
        {"L", "b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", Expect::kBadCombination},
        {"M", "b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kBadCombination},
        {"N", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250", Expect::kMalformed},
        {"O", "b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kMalformed},
        {"P", "b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kMalformed},
        {"Q", "b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kMalformed},
        {"R", "b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kMalformed},
        // S (length cap) is the handler's content_len check; the empty-body form of S reaches the parser.
        {"S", "", Expect::kMalformed},
        {"T", "b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", Expect::kOk},
        {"U", a + "&x=1", Expect::kOk},
        {"V", "b0h_ns=90&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", Expect::kOutOfRange},
        {"W", "b0h_ns=100&b0p_ns=800&b1h_ns=125&b1p_ns=800&rst_us=50", Expect::kOk},
        {"X", "b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800", Expect::kOk},
        {"Y", "b0h_ns=175&b0p_ns=1125&b1h_ns=125&b1p_ns=800&rst_us=280", Expect::kOk},
        {"Z", "b0h_ns=125&b0p_ns=800&b1h_ns=175&b1p_ns=1125&rst_us=280", Expect::kBadDutyOrder},
        {"AA", "b0h_ns=400&b0p_ns=1000&b1h_ns=500&b1p_ns=1250&rst_us=280", Expect::kBadDutyOrder},
        {"AB", "b0h_ns=500&b0p_ns=1000&b1h_ns=600&b1p_ns=1250&rst_us=280", Expect::kBadDutyOrder},
        {"AC", "b0h_ns=500&b0p_ns=1250&b1h_ns=500&b1p_ns=1000&rst_us=280", Expect::kOk},
        {"AD", "b0h_ns=600&b0p_ns=2000&b1h_ns=500&b1p_ns=1000&rst_us=280", Expect::kOk},
        {"AE", "b0h_ns=1200&b0p_ns=1250&b1h_ns=400&b1p_ns=1250&rst_us=280", Expect::kBadCombination},
        {"AF", "b0h_ns=1225&b0p_ns=2000&b1h_ns=400&b1p_ns=1250&rst_us=280", Expect::kOutOfRange},
    };
}

Expect Classify(const std::string &body)
{
    ws2812_timing_t timing = {};
    if (!ParseTunerForm(body.c_str(), &timing)) {
        return Expect::kMalformed;
    }
    switch (ValidateWs2812Timing(&timing)) {
    case WS2812_TIMING_OK: return Expect::kOk;
    case WS2812_TIMING_OUT_OF_RANGE: return Expect::kOutOfRange;
    case WS2812_TIMING_BAD_COMBINATION: return Expect::kBadCombination;
    case WS2812_TIMING_BAD_DUTY_ORDER: return Expect::kBadDutyOrder;
    }
    return Expect::kMalformed;
}

/** V4 with exact 64-bit products, independent of the firmware (section 7.3). */
bool V4Holds(const ws2812_timing_t &t)
{
    return static_cast<uint64_t>(t.bit0_high_ns) * t.bit1_period_ns <
           static_cast<uint64_t>(t.bit1_high_ns) * t.bit0_period_ns;
}

/** Page duty display of FR-8: permille = floor((high * 1000 + period / 2) / period). */
unsigned DisplayPermille(unsigned high_ns, unsigned period_ns) { return (high_ns * 1000 + period_ns / 2) / period_ns; }

}  // namespace

TEST_CASE("all 32 reference vectors A to AF give the section 7.5 decision", "[T-1][T-15][FR-15][FR-16]")
{
    const std::vector<ReferenceVector> vectors = AllVectors();
    REQUIRE(vectors.size() == 32);
    for (const ReferenceVector &vector : vectors) {
        INFO("vector " << vector.id << ": " << vector.body);
        REQUIRE(static_cast<int>(Classify(vector.body)) == static_cast<int>(vector.expected));
    }
}

TEST_CASE("V4 rejects B, D, Z, AA and AB as bad_duty_order; W, X, Y, AC and AD are valid", "[T-1][T-15][FR-16]")
{
    for (const auto &[id, expected] : std::vector<std::pair<std::string, Expect>>{
             {"B", Expect::kBadDutyOrder}, {"D", Expect::kBadDutyOrder}, {"Z", Expect::kBadDutyOrder},
             {"AA", Expect::kBadDutyOrder}, {"AB", Expect::kBadDutyOrder}, {"C", Expect::kOk}, {"W", Expect::kOk},
             {"X", Expect::kOk}, {"Y", Expect::kOk}, {"AC", Expect::kOk}, {"AD", Expect::kOk}}) {
        for (const ReferenceVector &vector : AllVectors()) {
            if (id == vector.id) {
                INFO(id);
                REQUIRE(static_cast<int>(Classify(vector.body)) == static_cast<int>(expected));
            }
        }
    }
}

TEST_CASE("precedence: M and AE are bad_combination (V3 wins), AF and V are out_of_range (range wins)", "[T-1][T-15][FR-16]")
{
    // Each of these also violates V4; the first failing rule decides.
    const ws2812_timing_t m = {1200, 1200, 800, 1250, 280};
    const ws2812_timing_t ae = {1200, 1250, 400, 1250, 280};
    const ws2812_timing_t af = {1225, 2000, 400, 1250, 280};
    REQUIRE_FALSE(V4Holds(m));
    REQUIRE_FALSE(V4Holds(ae));
    REQUIRE_FALSE(V4Holds(af));
    REQUIRE(ValidateWs2812Timing(&m) == WS2812_TIMING_BAD_COMBINATION);
    REQUIRE(ValidateWs2812Timing(&ae) == WS2812_TIMING_BAD_COMBINATION);
    REQUIRE(ValidateWs2812Timing(&af) == WS2812_TIMING_OUT_OF_RANGE);
}

TEST_CASE("Y and Z both display 15.6 % for each bit; the exact products decide (625 apart)", "[T-1][T-3][T-15][FR-10][FR-16]")
{
    const ws2812_timing_t y = {175, 1125, 125, 800, 280};
    const ws2812_timing_t z = {125, 800, 175, 1125, 280};
    REQUIRE(DisplayPermille(175, 1125) == 156);
    REQUIRE(DisplayPermille(125, 800) == 156);
    REQUIRE(175u * 800u == 140000u);
    REQUIRE(125u * 1125u == 140625u);
    REQUIRE(ValidateWs2812Timing(&y) == WS2812_TIMING_OK);              // 140,000 < 140,625
    REQUIRE(ValidateWs2812Timing(&z) == WS2812_TIMING_BAD_DUTY_ORDER);  // 140,625 >= 140,000
}

TEST_CASE("V4 sweep: product differences of -625, 0 and +625 over 20 sampled period pairs", "[T-3][T-15][FR-16]")
{
    // All four values are multiples of 25, so a difference is 625 * (b1h'*b0p' - b0h'*b1p') with x' = x / 25.
    // A difference of +/-625 therefore needs gcd(b0p', b1p') = 1, and then an equal product would need a high time
    // that is a multiple of its own period, which V3 forbids. No single period pair can show all three cases
    // (reported as a T-3 wording issue); the 20 pairs are split: 10 coprime pairs give -625 (reject) and +625
    // (accept), 10 non-coprime pairs give 0 (reject, equal duty).
    struct Pair { uint16_t b0p, b1p; bool coprime; };
    const Pair kPairs[] = {
        {800, 1125, true},   {1125, 800, true},   {1200, 1325, true},  {1300, 1225, true},  {1100, 1675, true},
        {800, 875, true},    {975, 850, true},    {1225, 800, true},   {1675, 1425, true},  {1975, 1700, true},
        {800, 800, false},   {1000, 1250, false}, {1250, 1000, false}, {1200, 1300, false}, {2000, 2000, false},
        {2000, 1000, false}, {1000, 2000, false}, {1500, 1750, false}, {1750, 1500, false}, {1100, 1650, false},
    };
    for (const Pair &pair : kPairs) {
        bool found[3] = {false, false, false};   // -625, 0, +625
        for (int b0h = TUNER_HIGH_MIN_NS; b0h <= TUNER_HIGH_MAX_NS && b0h + TUNER_MIN_LOW_NS <= pair.b0p; b0h += 25) {
            for (int b1h = TUNER_HIGH_MIN_NS; b1h <= TUNER_HIGH_MAX_NS && b1h + TUNER_MIN_LOW_NS <= pair.b1p; b1h += 25) {
                const int64_t diff = static_cast<int64_t>(b1h) * pair.b0p - static_cast<int64_t>(b0h) * pair.b1p;
                const int slot = diff == -625 ? 0 : diff == 0 ? 1 : diff == 625 ? 2 : -1;
                if (slot < 0) {
                    continue;
                }
                found[slot] = true;
                const ws2812_timing_t timing = {static_cast<uint16_t>(b0h), pair.b0p, static_cast<uint16_t>(b1h),
                                                pair.b1p, 280};
                INFO("b0h=" << b0h << " b0p=" << pair.b0p << " b1h=" << b1h << " b1p=" << pair.b1p << " diff=" << diff);
                REQUIRE(ValidateWs2812Timing(&timing) ==
                        (slot == 2 ? WS2812_TIMING_OK : WS2812_TIMING_BAD_DUTY_ORDER));
            }
        }
        INFO("pair " << pair.b0p << "/" << pair.b1p);
        REQUIRE(found[0] == pair.coprime);
        REQUIRE(found[2] == pair.coprime);
        REQUIRE(found[1] == !pair.coprime);
    }
}

TEST_CASE("V4 exhaustive cross-check against exact 64-bit products on a coarse grid", "[T-3][T-15][FR-16]")
{
    long checked = 0;
    for (uint16_t b0p = 800; b0p <= 2000; b0p += 75) {
        for (uint16_t b1p = 800; b1p <= 2000; b1p += 75) {
            for (uint16_t b0h = 100; b0h <= 1200 && b0h + 100 <= b0p; b0h += 50) {
                for (uint16_t b1h = 100; b1h <= 1200 && b1h + 100 <= b1p; b1h += 50) {
                    const ws2812_timing_t timing = {b0h, b0p, b1h, b1p, 280};
                    const ws2812_timing_result_t expected =
                        V4Holds(timing) ? WS2812_TIMING_OK : WS2812_TIMING_BAD_DUTY_ORDER;
                    if (ValidateWs2812Timing(&timing) != expected) {
                        FAIL("b0h=" << b0h << " b0p=" << b0p << " b1h=" << b1h << " b1p=" << b1p);
                    }
                    ++checked;
                }
            }
        }
    }
    REQUIRE(checked > 50000);
}

TEST_CASE("V4 maximum products are computed without overflow", "[T-3][T-15][FR-16]")
{
    // Largest valid products: 1,200 x 2,000 = 2,400,000 each side. b1h 1,200 / b1p 1,300 against b0h 1,175 / b0p 1,275
    // differ by 2,500 near the top of the range.
    const ws2812_timing_t top_valid = {1175, 1275, 1200, 1300, 800};
    REQUIRE(V4Holds(top_valid));
    REQUIRE(ValidateWs2812Timing(&top_valid) == WS2812_TIMING_OK);
    const ws2812_timing_t top_equal = {1200, 2000, 1200, 2000, 800};   // 2,400,000 >= 2,400,000
    REQUIRE(ValidateWs2812Timing(&top_equal) == WS2812_TIMING_BAD_DUTY_ORDER);
    // A 16-bit product would wrap: 1,000 x 2,000 = 2,000,000 vs 1,200 x 2,000 = 2,400,000 (vector X).
    const ws2812_timing_t x = {1000, 2000, 1200, 2000, 800};
    REQUIRE(ValidateWs2812Timing(&x) == WS2812_TIMING_OK);
}
