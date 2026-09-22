/**
 * @file test_ws2812_logging.cpp
 * @brief Host tests for LogWs2812Timing() and LogWs2812Rejection() (SPEC-003 T-2;
 *        FR-17, FR-18, NFR-15), using the FFF fake of LogWrite() from mocks/log_fakes.c.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "log_fakes.h"
#include "logging.h"
#include "ws2812_timing.h"
}

namespace {

ws2812_timing_t Timing(uint16_t b0h, uint16_t b0p, uint16_t b1h, uint16_t b1p, uint16_t rst)
{
    return {b0h, b0p, b1h, b1p, rst};
}

/** The single line captured since the last TestLogReset(), without the "[L<level> <module>] " prefix. */
std::string LastMessage()
{
    std::string text = TestLogText();
    size_t bracket_end = text.find("] ");
    REQUIRE(bracket_end != std::string::npos);
    size_t newline = text.find('\n', bracket_end);
    return text.substr(bracket_end + 2, newline - (bracket_end + 2));
}

}  // namespace

// ---- FR-17: accepted timing sets, vectors A-D, E -------------------------------------------------------------------

TEST_CASE("vector A: the Info log line matches FR-17 exactly", "[T-2][FR-17]")
{
    TestLogReset();
    ws2812_timing_t timing = Timing(400, 1250, 800, 1250, 280);
    LogWs2812Timing(&timing);

    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(LastMessage() ==
            "tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280");
    REQUIRE(std::string(TestLogText()).substr(0, 4) == "[L1 ");   // LOG_LEVEL_INFO == 1
}

TEST_CASE("vector B: the Info log line for the minimum-edge timing set", "[T-2][FR-17]")
{
    TestLogReset();
    ws2812_timing_t timing = Timing(100, 800, 100, 800, 50);
    LogWs2812Timing(&timing);

    REQUIRE(LastMessage() ==
            "tuner received: bit0 high_ns=100 period_ns=800; bit1 high_ns=100 period_ns=800; reset_us=50");
}

TEST_CASE("vector C: the Info log line for the low-time-edge timing set", "[T-2][FR-17]")
{
    TestLogReset();
    ws2812_timing_t timing = Timing(1075, 1200, 1100, 1200, 280);
    LogWs2812Timing(&timing);

    REQUIRE(LastMessage() ==
            "tuner received: bit0 high_ns=1075 period_ns=1200; bit1 high_ns=1100 period_ns=1200; reset_us=280");
}

TEST_CASE("vector D: the longest Info log line is exactly 96 characters", "[T-2][FR-17]")
{
    TestLogReset();
    ws2812_timing_t timing = Timing(1200, 2000, 1000, 2000, 800);
    LogWs2812Timing(&timing);

    const std::string message = LastMessage();
    REQUIRE(message ==
            "tuner received: bit0 high_ns=1200 period_ns=2000; bit1 high_ns=1000 period_ns=2000; reset_us=800");
    REQUIRE(message.size() == 96);
}

TEST_CASE("vector E: leading zeros in the parsed values produce vector A's log line", "[T-2][FR-17]")
{
    ws2812_timing_t timing = {};
    REQUIRE(ParseTunerForm("b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", &timing));

    TestLogReset();
    LogWs2812Timing(&timing);

    REQUIRE(LastMessage() ==
            "tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280");
}

TEST_CASE("the Info line never contains low_ns or duty", "[T-2][FR-17]")
{
    TestLogReset();
    ws2812_timing_t timing = Timing(1200, 2000, 1000, 2000, 800);   // widest low-time spread
    LogWs2812Timing(&timing);

    const std::string message = LastMessage();
    REQUIRE(message.find("low_ns") == std::string::npos);
    REQUIRE(message.find("duty") == std::string::npos);
}

// ---- FR-18: rejection reasons ----------------------------------------------------------------------------------------

TEST_CASE("a malformed request logs the malformed reason at Warning", "[T-2][FR-18]")
{
    TestLogReset();
    LogWs2812Rejection(WS2812_REJECT_MALFORMED);

    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(LastMessage() == "tuner request rejected: reason=malformed");
    REQUIRE(std::string(TestLogText()).substr(0, 4) == "[L2 ");   // LOG_LEVEL_WARNING == 2
}

TEST_CASE("an out-of-range request logs the out_of_range reason at Warning", "[T-2][FR-18]")
{
    TestLogReset();
    LogWs2812Rejection(WS2812_REJECT_OUT_OF_RANGE);

    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LastMessage() == "tuner request rejected: reason=out_of_range");
}

TEST_CASE("a bad-combination request logs the bad_combination reason at Warning", "[T-2][FR-18]")
{
    TestLogReset();
    LogWs2812Rejection(WS2812_REJECT_BAD_COMBINATION);

    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LastMessage() == "tuner request rejected: reason=bad_combination");
}

TEST_CASE("a rejection never emits the FR-17 Info line", "[T-2][FR-18]")
{
    TestLogReset();
    LogWs2812Rejection(WS2812_REJECT_OUT_OF_RANGE);

    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(std::string(TestLogText()).find("tuner received:") == std::string::npos);
}
