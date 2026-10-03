/**
 * @file test_led_pure.cpp
 * @brief Host tests for the SPEC-004 pure functions (NFR-10, NFR-14):
 *        T-1 Ws2812NsToTicks() (FR-9), T-2 BuildLedPixelPattern() (FR-12, FR-14, FR-15),
 *        T-12 idle-threshold/glitch window (FR-22), T-13 Ws2812TicksToNs()/DecodeWs2812Symbol()
 *        (FR-26, FR-27), T-14 AggregateWs2812Pulses() (FR-28), plus the FR-23 capture constants.
 *
 * led_controller.c and rmt_pulse_monitor.c are linked unmodified; none of the functions tested
 * here touches a FreeRTOS/RMT fake (checked once below).
 */
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>
#include <vector>

#include "fff.h"
extern "C" {
#include "freertos_fakes.h"
#include "led_controller.h"
#include "log_fake_access.h"
#include "log_fakes.h"
#include "rmt_fakes.h"
#include "rmt_pulse_monitor.h"
}

namespace {

constexpr uint32_t kTickNs = 25;

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};    // SPEC-003 defaults / vector A
const ws2812_timing_t kVectorB = {100, 800, 100, 800, 50};       // minimum edge, equal highs
const ws2812_timing_t kVectorD = {1200, 2000, 1000, 2000, 800};  // maximum edge, inverted highs
// B and D are pure-function inputs only since SPEC-003 V4 (2026-10-03); AC and AD are their tuner-reachable forms.
const ws2812_timing_t kVectorAC = {500, 1250, 500, 1000, 280};   // equal highs, shorter bit-1 period
const ws2812_timing_t kVectorAD = {600, 2000, 500, 1000, 280};   // inverted highs, valid duty order

using Pixels = std::array<uint8_t, LED_CONTROLLER_PIXEL_BYTES>;
using Capture = std::vector<rmt_symbol_word_t>;

Pixels FixedPattern()
{
    Pixels pixels{};
    BuildLedPixelPattern(pixels.data());
    return pixels;
}

bool ExpectedBit(const Pixels &pixels, size_t index)
{
    return ((pixels[index / 8] >> (7 - index % 8)) & 0x01) != 0;
}

rmt_symbol_word_t Symbol(uint32_t high_ticks, uint32_t low_ticks, uint32_t level0 = 1)
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = high_ticks & 0x7FFF;
    symbol.level0 = level0 & 0x1;
    symbol.duration1 = low_ticks & 0x7FFF;
    symbol.level1 = 0;
    return symbol;
}

/** An ideal 144-symbol RX capture of @p pixels sent with @p timing: the final duration1 is the end marker (0). */
Capture IdealCapture(const ws2812_timing_t &timing, const Pixels &pixels)
{
    Capture capture;
    for (size_t index = 0; index < RMT_PULSE_MONITOR_DATA_SYMBOLS; ++index) {
        const bool bit = ExpectedBit(pixels, index);
        const uint32_t high_ns = bit ? timing.bit1_high_ns : timing.bit0_high_ns;
        const uint32_t period_ns = bit ? timing.bit1_period_ns : timing.bit0_period_ns;
        capture.push_back(Symbol(high_ns / kTickNs, (period_ns - high_ns) / kTickNs));
    }
    capture.back().duration1 = 0;
    return capture;
}

bool Aggregate(const Capture &capture, const ws2812_timing_t &timing, const Pixels &pixels, ws2812_pulse_stats_t &stats,
               size_t count = RMT_PULSE_MONITOR_DATA_SYMBOLS)
{
    return AggregateWs2812Pulses(capture.data(), count, &timing, pixels.data(), pixels.size(), kTickNs, &stats);
}

void ResetFakes()
{
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
}

}  // namespace

// ---- T-1: Ws2812NsToTicks() (FR-9) -------------------------------------------------------------------------------

TEST_CASE("Ws2812NsToTicks: SPEC-004 section 7.2 worked example", "[T-1][FR-9]")
{
    REQUIRE(Ws2812NsToTicks(400, kTickNs) == 16);
    REQUIRE(Ws2812NsToTicks(850, kTickNs) == 34);
    REQUIRE(Ws2812NsToTicks(800, kTickNs) == 32);
    REQUIRE(Ws2812NsToTicks(450, kTickNs) == 18);
    REQUIRE(Ws2812NsToTicks(280u * 1000u, kTickNs) == 11200);
}

TEST_CASE("Ws2812NsToTicks: T-1 boundary values", "[T-1][FR-9]")
{
    REQUIRE(Ws2812NsToTicks(1200, kTickNs) == 48);
    REQUIRE(Ws2812NsToTicks(800000, kTickNs) == 32000);   // TUNER_RESET_MAX_US
    REQUIRE(Ws2812NsToTicks(0, kTickNs) == 0);
}

TEST_CASE("Ws2812NsToTicks: SPEC-003 range edges", "[T-1][FR-9][FR-11]")
{
    REQUIRE(Ws2812NsToTicks(TUNER_HIGH_MIN_NS, kTickNs) == 4);
    REQUIRE(Ws2812NsToTicks(TUNER_HIGH_MAX_NS, kTickNs) == 48);
    REQUIRE(Ws2812NsToTicks(TUNER_PERIOD_MIN_NS, kTickNs) == 32);
    REQUIRE(Ws2812NsToTicks(TUNER_PERIOD_MAX_NS, kTickNs) == 80);
    REQUIRE(Ws2812NsToTicks(TUNER_MIN_LOW_NS, kTickNs) == 4);
    REQUIRE(Ws2812NsToTicks(TUNER_PERIOD_MAX_NS - TUNER_HIGH_MIN_NS, kTickNs) == 76);   // longest low time
    REQUIRE(Ws2812NsToTicks(TUNER_RESET_MIN_US * 1000u, kTickNs) == 2000);
    REQUIRE(Ws2812NsToTicks(TUNER_RESET_MAX_US * 1000u, kTickNs) == 32000);
    REQUIRE(Ws2812NsToTicks(TUNER_RESET_MAX_US * 1000u, kTickNs) <= 0x7FFF);   // one 15-bit RMT field (FR-11)
}

TEST_CASE("Ws2812NsToTicks: round-half-up for non-multiples of the tick", "[T-1][FR-9]")
{
    // tick_ns = 25, so the half is 12.5 ns: 12 rounds down, 13 rounds up.
    REQUIRE(Ws2812NsToTicks(12, kTickNs) == 0);
    REQUIRE(Ws2812NsToTicks(13, kTickNs) == 1);
    REQUIRE(Ws2812NsToTicks(37, kTickNs) == 1);
    REQUIRE(Ws2812NsToTicks(38, kTickNs) == 2);
    REQUIRE(Ws2812NsToTicks(410, kTickNs) == 16);   // SPEC-003 vector G value: 16.4 -> 16
    REQUIRE(Ws2812NsToTicks(413, kTickNs) == 17);   // 16.52 -> 17
    // An even tick has an exact half: it rounds up.
    REQUIRE(Ws2812NsToTicks(5, 10) == 1);
    REQUIRE(Ws2812NsToTicks(4, 10) == 0);
    REQUIRE(Ws2812NsToTicks(15, 10) == 2);
}

TEST_CASE("Ws2812NsToTicks is exact for every SPEC-003-valid value and Ws2812TicksToNs inverts it", "[T-1][T-13][FR-9][FR-26]")
{
    for (uint32_t ns = TUNER_HIGH_MIN_NS; ns <= TUNER_PERIOD_MAX_NS; ns += TUNER_STEP_NS) {
        INFO("ns = " << ns);
        const uint16_t ticks = Ws2812NsToTicks(ns, kTickNs);
        REQUIRE(ticks * kTickNs == ns);
        REQUIRE(Ws2812TicksToNs(ticks, kTickNs) == ns);
    }
    for (uint32_t us = TUNER_RESET_MIN_US; us <= TUNER_RESET_MAX_US; us += TUNER_RST_STEP_US) {
        INFO("reset_us = " << us);
        const uint16_t ticks = Ws2812NsToTicks(us * 1000u, kTickNs);
        REQUIRE(Ws2812TicksToNs(ticks, kTickNs) == us * 1000u);
    }
}

// ---- T-2: BuildLedPixelPattern() (FR-12, FR-14, FR-15) -----------------------------------------------------------

TEST_CASE("BuildLedPixelPattern fills exactly the section 7.1 GRB table", "[T-2][FR-12][FR-14][FR-15]")
{
    REQUIRE(LED_CONTROLLER_LED_COUNT == 6);
    REQUIRE(LED_CONTROLLER_PIXEL_BYTES == 18);
    REQUIRE(LED_CONTROLLER_CHANNEL_VALUE == 32);

    uint8_t buffer[LED_CONTROLLER_PIXEL_BYTES + 2];
    std::memset(buffer, 0xAA, sizeof(buffer));
    BuildLedPixelPattern(buffer);

    const uint8_t kExpected[LED_CONTROLLER_PIXEL_BYTES] = {
        0x00, 0x20, 0x00,   // LED 0 red   (G, R, B)
        0x00, 0x20, 0x00,   // LED 1 red
        0x20, 0x00, 0x00,   // LED 2 green
        0x20, 0x00, 0x00,   // LED 3 green
        0x00, 0x00, 0x20,   // LED 4 blue
        0x00, 0x00, 0x20,   // LED 5 blue
    };
    for (size_t index = 0; index < LED_CONTROLLER_PIXEL_BYTES; ++index) {
        INFO("byte " << index);
        REQUIRE(buffer[index] == kExpected[index]);
    }
    REQUIRE(buffer[LED_CONTROLLER_PIXEL_BYTES] == 0xAA);       // nothing written past 18 bytes
    REQUIRE(buffer[LED_CONTROLLER_PIXEL_BYTES + 1] == 0xAA);
}

TEST_CASE("BuildLedPixelPattern: one active channel per LED at LED_CONTROLLER_CHANNEL_VALUE", "[T-2][FR-15]")
{
    const Pixels pixels = FixedPattern();
    for (size_t led = 0; led < LED_CONTROLLER_LED_COUNT; ++led) {
        INFO("LED " << led);
        int active = 0;
        for (size_t channel = 0; channel < 3; ++channel) {
            const uint8_t value = pixels[led * 3 + channel];
            REQUIRE((value == 0 || value == LED_CONTROLLER_CHANNEL_VALUE));
            active += (value != 0);
        }
        REQUIRE(active == 1);
    }
}

TEST_CASE("BuildLedPixelPattern is constant and touches no driver", "[T-2][FR-14][NFR-10]")
{
    ResetFakes();
    const Pixels first = FixedPattern();
    const Pixels second = FixedPattern();
    REQUIRE(first == second);
    (void)Ws2812NsToTicks(400, kTickNs);
    REQUIRE(fff.call_history_idx == 0);   // no RMT, FreeRTOS or log fake was called
}

// ---- T-12: idle threshold / glitch filter window (FR-22) ---------------------------------------------------------

TEST_CASE("the idle threshold lies strictly inside the derived 76..2,000 tick window", "[T-12][FR-22]")
{
    const uint32_t longest_low_ticks = (TUNER_PERIOD_MAX_NS - TUNER_HIGH_MIN_NS) / RMT_PULSE_MONITOR_TICK_NS;
    const uint32_t shortest_reset_ticks = (TUNER_RESET_MIN_US * 1000u) / RMT_PULSE_MONITOR_TICK_NS;
    const uint32_t idle_ticks = RMT_PULSE_MONITOR_IDLE_NS / RMT_PULSE_MONITOR_TICK_NS;

    REQUIRE(longest_low_ticks == 76);
    REQUIRE(shortest_reset_ticks == 2000);
    REQUIRE(RMT_PULSE_MONITOR_IDLE_NS == 25000);
    REQUIRE(idle_ticks == 1000);
    REQUIRE(longest_low_ticks < idle_ticks);
    REQUIRE(idle_ticks < shortest_reset_ticks);
}

TEST_CASE("the glitch filter is below the shortest valid pulse", "[T-12][FR-22]")
{
    // The glitch filter cannot lie in the 76..2,000 window (it filters short pulses); the spec's own
    // rationale (section 7.5) is "below the SPEC-003 100 ns minimum pulse width".
    const uint32_t shortest_pulse_ns = TUNER_HIGH_MIN_NS < TUNER_MIN_LOW_NS ? TUNER_HIGH_MIN_NS : TUNER_MIN_LOW_NS;
    REQUIRE(RMT_PULSE_MONITOR_GLITCH_NS == 50);
    REQUIRE(RMT_PULSE_MONITOR_GLITCH_NS > 0);
    REQUIRE(RMT_PULSE_MONITOR_GLITCH_NS < shortest_pulse_ns);
    REQUIRE(RMT_PULSE_MONITOR_GLITCH_NS < RMT_PULSE_MONITOR_IDLE_NS);
}

TEST_CASE("RX constants match the TX side and the 144-symbol capture", "[T-12][FR-19][FR-23][FR-31]")
{
    REQUIRE(RMT_PULSE_MONITOR_RX_GPIO_NUM == 4);
    REQUIRE(RMT_PULSE_MONITOR_RX_GPIO_NUM != LED_CONTROLLER_GPIO_NUM);
    REQUIRE(RMT_PULSE_MONITOR_RESOLUTION_HZ == LED_CONTROLLER_RESOLUTION_HZ);
    REQUIRE(RMT_PULSE_MONITOR_TICK_NS == LED_CONTROLLER_TICK_NS);
    REQUIRE(1000000000u / LED_CONTROLLER_RESOLUTION_HZ == LED_CONTROLLER_TICK_NS);
    REQUIRE(RMT_PULSE_MONITOR_DATA_SYMBOLS == 144);
    REQUIRE(RMT_PULSE_MONITOR_DATA_SYMBOLS == LED_CONTROLLER_PIXEL_BYTES * 8);
    REQUIRE(RMT_PULSE_MONITOR_SYMBOL_CAPACITY == 144);
    REQUIRE(sizeof(rmt_symbol_word_t) == 4);
    REQUIRE(RMT_PULSE_MONITOR_BUFFER_BYTES == 576);
    REQUIRE(RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES == LED_CONTROLLER_PIXEL_BYTES);
    REQUIRE(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS == 20);
    REQUIRE(pdMS_TO_TICKS(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS) >= 2);   // 2 ticks at 100 Hz
}

TEST_CASE("the TX frame completion timeout satisfies NFR-2", "[FR-13][NFR-2]")
{
    REQUIRE(LED_CONTROLLER_FRAME_TIMEOUT_MS == 20);
    REQUIRE(LED_CONTROLLER_FRAME_TIMEOUT_MS * 1000 >= 1200);   // >= 1.2 ms
    REQUIRE(LED_CONTROLLER_FRAME_TIMEOUT_MS <= 20);
    REQUIRE(pdMS_TO_TICKS(LED_CONTROLLER_FRAME_TIMEOUT_MS) >= 2);   // >= 2 ticks at CONFIG_FREERTOS_HZ = 100
}

// ---- T-13: Ws2812TicksToNs() and DecodeWs2812Symbol() (FR-26, FR-27) ---------------------------------------------

TEST_CASE("Ws2812TicksToNs multiplies ticks by the tick length", "[T-13][FR-26]")
{
    REQUIRE(Ws2812TicksToNs(0, kTickNs) == 0);
    REQUIRE(Ws2812TicksToNs(16, kTickNs) == 400);
    REQUIRE(Ws2812TicksToNs(34, kTickNs) == 850);
    REQUIRE(Ws2812TicksToNs(32000, kTickNs) == 800000);
    REQUIRE(Ws2812TicksToNs(0x7FFF, kTickNs) == 819175);
}

TEST_CASE("DecodeWs2812Symbol: the exact tie 600 ns is bit 0, 601 ns is bit 1", "[T-13][FR-27]")
{
    // 400/800 ns commanded highs: 600 ns is equidistant (200/200) -> tie rule -> bit 0;
    // 601 ns is strictly nearer 800 (199 < 201) -> bit 1. With 1 ns ticks both are representable.
    REQUIRE(DecodeWs2812Symbol(Symbol(600, 10), 400, 800, 1) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(601, 10), 400, 800, 1) == WS2812_SYMBOL_BIT1);
    REQUIRE(DecodeWs2812Symbol(Symbol(599, 10), 400, 800, 1) == WS2812_SYMBOL_BIT0);
    // At the real 25 ns tick: 24 ticks = 600 ns (tie) -> bit 0, 25 ticks = 625 ns -> bit 1.
    REQUIRE(DecodeWs2812Symbol(Symbol(24, 34), 400, 800, kTickNs) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(25, 34), 400, 800, kTickNs) == WS2812_SYMBOL_BIT1);
    REQUIRE(DecodeWs2812Symbol(Symbol(16, 34), 400, 800, kTickNs) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(32, 18), 400, 800, kTickNs) == WS2812_SYMBOL_BIT1);
    // Far outside both commanded values the nearest one still wins.
    REQUIRE(DecodeWs2812Symbol(Symbol(0, 10), 400, 800, 1) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(5000, 10), 400, 800, 1) == WS2812_SYMBOL_BIT1);
}

TEST_CASE("DecodeWs2812Symbol: level0 != 1 is unclassifiable", "[T-13][FR-27][FR-32]")
{
    REQUIRE(DecodeWs2812Symbol(Symbol(16, 34, 0), 400, 800, kTickNs) == WS2812_SYMBOL_INVALID);
    REQUIRE(DecodeWs2812Symbol(Symbol(32, 18, 0), 400, 800, kTickNs) == WS2812_SYMBOL_INVALID);
    REQUIRE(DecodeWs2812Symbol(Symbol(0, 0, 0), 400, 800, kTickNs) == WS2812_SYMBOL_INVALID);
    // Also with inverted and with equal commanded highs: the level check comes first.
    REQUIRE(DecodeWs2812Symbol(Symbol(48, 32, 0), 1200, 1000, kTickNs) == WS2812_SYMBOL_INVALID);
    REQUIRE(DecodeWs2812Symbol(Symbol(4, 28, 0), 100, 100, kTickNs) == WS2812_SYMBOL_INVALID);
}

TEST_CASE("DecodeWs2812Symbol: the nearest commanded high time of the applied set wins", "[T-13][FR-27]")
{
    // Vector C: 1075/1100 ns -> the tie point is 1087.5 ns, so 1087 is nearer bit 0 and 1088 nearer bit 1.
    REQUIRE(DecodeWs2812Symbol(Symbol(1087, 0), 1075, 1100, 1) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(1088, 0), 1075, 1100, 1) == WS2812_SYMBOL_BIT1);
    REQUIRE(DecodeWs2812Symbol(Symbol(43, 5), 1075, 1100, kTickNs) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(44, 4), 1075, 1100, kTickNs) == WS2812_SYMBOL_BIT1);
}

TEST_CASE("DecodeWs2812Symbol: inverted timing (bit-0 high > bit-1 high) classifies by the nearer value", "[T-13][FR-27]")
{
    // T-13: bit0_high_ns = 1,200, bit1_high_ns = 1,000.
    REQUIRE(DecodeWs2812Symbol(Symbol(1200, 10), 1200, 1000, 1) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(1000, 10), 1200, 1000, 1) == WS2812_SYMBOL_BIT1);
    REQUIRE(DecodeWs2812Symbol(Symbol(1100, 10), 1200, 1000, 1) == WS2812_SYMBOL_BIT0);   // the tie is bit 0
    REQUIRE(DecodeWs2812Symbol(Symbol(1101, 10), 1200, 1000, 1) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(1099, 10), 1200, 1000, 1) == WS2812_SYMBOL_BIT1);
    // SPEC-003 vector D at the 25 ns tick: 48 ticks = 1,200 ns, 40 ticks = 1,000 ns, 44 ticks = 1,100 ns (tie).
    REQUIRE(DecodeWs2812Symbol(Symbol(48, 32), 1200, 1000, kTickNs) == WS2812_SYMBOL_BIT0);
    REQUIRE(DecodeWs2812Symbol(Symbol(40, 40), 1200, 1000, kTickNs) == WS2812_SYMBOL_BIT1);
    REQUIRE(DecodeWs2812Symbol(Symbol(44, 36), 1200, 1000, kTickNs) == WS2812_SYMBOL_BIT0);
}

TEST_CASE("DecodeWs2812Symbol: equal commanded high times classify every symbol as bit 0", "[T-13][FR-27]")
{
    for (uint32_t high_ticks : {0u, 1u, 4u, 5u, 40u, 0x7FFFu}) {
        INFO("high_ticks = " << high_ticks);
        REQUIRE(DecodeWs2812Symbol(Symbol(high_ticks, 28), 100, 100, kTickNs) == WS2812_SYMBOL_BIT0);
    }
}

// ---- T-14: AggregateWs2812Pulses() (FR-28) -----------------------------------------------------------------------

TEST_CASE("aggregate: vector A ideal capture, all 144 bits match", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(IdealCapture(kVectorA, pixels), kVectorA, pixels, stats));

    REQUIRE(stats.bit0_first_high_ns == 400);
    REQUIRE(stats.bit0_first_low_ns == 850);
    REQUIRE(stats.bit1_first_high_ns == 800);
    REQUIRE(stats.bit1_first_low_ns == 450);
    REQUIRE(stats.bit0_high_min_ns == 400);
    REQUIRE(stats.bit0_high_max_ns == 400);
    REQUIRE(stats.bit0_high_avg_ns == 400);
    REQUIRE(stats.bit1_high_min_ns == 800);
    REQUIRE(stats.bit1_high_max_ns == 800);
    REQUIRE(stats.bit1_high_avg_ns == 800);
    REQUIRE(stats.match_count == 144);
}

TEST_CASE("aggregate: a flipped bit is counted as a mismatch", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorA, pixels);
    REQUIRE_FALSE(ExpectedBit(pixels, 0));
    capture[0] = Symbol(32, 18);   // expected 0, received a bit-1 shape
    REQUIRE(ExpectedBit(pixels, 10));
    capture[10] = Symbol(16, 34);  // expected 1, received a bit-0 shape

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.match_count == 142);
    // The flipped symbol at index 0 is now the first bit-1 example.
    REQUIRE(stats.bit1_first_high_ns == 800);
    REQUIRE(stats.bit1_first_low_ns == 450);
}

TEST_CASE("aggregate: min/max and round-half-up average of the high times", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();   // 138 zero bits, 6 one bits
    Capture capture = IdealCapture(kVectorA, pixels);
    int zeros_seen = 0;
    int ones_seen = 0;
    for (size_t index = 0; index < capture.size(); ++index) {
        if (ExpectedBit(pixels, index)) {
            // bit 1 highs: 32, 32, 32, 32, 32, 35 ticks -> sum 4,875 ns / 6 = 812.5 -> 813
            capture[index].duration0 = (++ones_seen == 6) ? 35 : 32;
        } else {
            // bit 0 highs: 69 x 16 ticks and 69 x 17 ticks -> (69 x 400 + 69 x 425) / 138 = 412.5 -> 413
            capture[index].duration0 = (zeros_seen++ % 2 == 0) ? 16 : 17;
        }
    }
    REQUIRE(zeros_seen == 138);
    REQUIRE(ones_seen == 6);

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.bit0_high_min_ns == 400);
    REQUIRE(stats.bit0_high_max_ns == 425);
    REQUIRE(stats.bit0_high_avg_ns == 413);
    REQUIRE(stats.bit1_high_min_ns == 800);
    REQUIRE(stats.bit1_high_max_ns == 875);
    REQUIRE(stats.bit1_high_avg_ns == 813);
    REQUIRE(stats.match_count == 144);
}

TEST_CASE("aggregate: the first examples come from the first classified symbol of each kind", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorA, pixels);
    capture[0] = Symbol(17, 33);      // first bit 0: 425 / 825 ns
    capture[1] = Symbol(15, 35);      // later bit 0s differ
    REQUIRE(ExpectedBit(pixels, 10));  // first bit 1 of the pattern (LED 0 byte 1 = 0x20)
    capture[10] = Symbol(31, 19);     // 775 / 475 ns

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.bit0_first_high_ns == 425);
    REQUIRE(stats.bit0_first_low_ns == 825);
    REQUIRE(stats.bit1_first_high_ns == 775);
    REQUIRE(stats.bit1_first_low_ns == 475);
}

TEST_CASE("aggregate: the final symbol is never a first-bit example (only bit 1 is last)", "[T-14][FR-23][FR-28]")
{
    Pixels pixels{};
    pixels[LED_CONTROLLER_PIXEL_BYTES - 1] = 0x01;   // bits 0..142 are 0, bit 143 (the last) is 1
    const Capture capture = IdealCapture(kVectorA, pixels);
    REQUIRE(capture.back().duration1 == 0);          // RMT end marker

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.bit1_first_high_ns == 0);          // not taken from the final symbol
    REQUIRE(stats.bit1_first_low_ns == 0);
    REQUIRE(stats.bit1_high_min_ns == 800);          // but it still counts for the statistics
    REQUIRE(stats.bit1_high_max_ns == 800);
    REQUIRE(stats.bit1_high_avg_ns == 800);
    REQUIRE(stats.bit0_first_high_ns == 400);
    REQUIRE(stats.bit0_first_low_ns == 850);
    REQUIRE(stats.match_count == 144);               // and for the match count
}

TEST_CASE("aggregate: the final symbol is never a first-bit example (only bit 0 is last)", "[T-14][FR-23][FR-28]")
{
    Pixels pixels;
    pixels.fill(0xFF);
    pixels[LED_CONTROLLER_PIXEL_BYTES - 1] = 0xFE;   // bits 0..142 are 1, bit 143 (the last) is 0
    const Capture capture = IdealCapture(kVectorA, pixels);

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.bit0_first_high_ns == 0);
    REQUIRE(stats.bit0_first_low_ns == 0);
    REQUIRE(stats.bit0_high_min_ns == 400);
    REQUIRE(stats.bit0_high_max_ns == 400);
    REQUIRE(stats.bit0_high_avg_ns == 400);
    REQUIRE(stats.bit1_first_high_ns == 800);
    REQUIRE(stats.bit1_first_low_ns == 450);
    REQUIRE(stats.match_count == 144);
}

TEST_CASE("aggregate: a kind that never occurs reports 0 for all its values", "[T-14][FR-28]")
{
    const Pixels pixels{};   // all zeros
    ws2812_pulse_stats_t stats;
    std::memset(&stats, 0x5A, sizeof(stats));
    REQUIRE(Aggregate(IdealCapture(kVectorA, pixels), kVectorA, pixels, stats));
    REQUIRE(stats.bit1_first_high_ns == 0);
    REQUIRE(stats.bit1_first_low_ns == 0);
    REQUIRE(stats.bit1_high_min_ns == 0);
    REQUIRE(stats.bit1_high_max_ns == 0);
    REQUIRE(stats.bit1_high_avg_ns == 0);
    REQUIRE(stats.match_count == 144);
}

TEST_CASE("aggregate: an unclassifiable symbol is excluded from the stats and counted as a mismatch", "[T-14][FR-28][FR-32]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorA, pixels);
    capture[3] = Symbol(1000, 5, 0);   // level0 = 0, with a huge duration that would skew max/avg

    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    REQUIRE(stats.match_count == 143);
    REQUIRE(stats.bit0_high_max_ns == 400);
    REQUIRE(stats.bit0_high_avg_ns == 400);
    REQUIRE(stats.bit1_high_max_ns == 800);
}

TEST_CASE("aggregate: a symbol count other than 144 is rejected", "[T-14][FR-28][FR-32]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorA, pixels);
    capture.push_back(Symbol(16, 0));   // room for a 145-entry call
    ws2812_pulse_stats_t stats;
    for (size_t count : {size_t{0}, size_t{1}, size_t{143}, size_t{145}}) {
        INFO("count = " << count);
        REQUIRE_FALSE(Aggregate(capture, kVectorA, pixels, stats, count));
    }
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats, 144));
}

TEST_CASE("aggregate: NULL arguments are rejected", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    const Capture capture = IdealCapture(kVectorA, pixels);
    ws2812_pulse_stats_t stats;
    REQUIRE_FALSE(AggregateWs2812Pulses(nullptr, 144, &kVectorA, pixels.data(), 18, kTickNs, &stats));
    REQUIRE_FALSE(AggregateWs2812Pulses(capture.data(), 144, nullptr, pixels.data(), 18, kTickNs, &stats));
    REQUIRE_FALSE(AggregateWs2812Pulses(capture.data(), 144, &kVectorA, nullptr, 18, kTickNs, &stats));
    REQUIRE_FALSE(AggregateWs2812Pulses(capture.data(), 144, &kVectorA, pixels.data(), 18, kTickNs, nullptr));
}

TEST_CASE("aggregate: bits beyond a shorter expected buffer are expected to be 0", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    const Capture capture = IdealCapture(kVectorA, pixels);
    ws2812_pulse_stats_t stats;
    // Only LED 0 (3 bytes) given: its one set bit matches; LEDs 1-5 carry 5 set bits now expected as 0.
    REQUIRE(AggregateWs2812Pulses(capture.data(), 144, &kVectorA, pixels.data(), 3, kTickNs, &stats));
    REQUIRE(stats.match_count == 139);
}

TEST_CASE("aggregate: vector B (equal highs) reports the match as not available, stats by expected bit", "[T-14][FR-27][FR-28]")
{
    // FR-28: bit0_high_ns == bit1_high_ns (100 ns) -> bits cannot be told apart from timing.
    const Pixels pixels = FixedPattern();   // 138 zero bits, 6 one bits
    Capture capture = IdealCapture(kVectorB, pixels);
    // Make the partition visible: expected-1 symbols 5 ticks high (125 ns), expected-0 symbols 4 ticks (100 ns).
    // Both still decode as bit 0 (tie rule / nearest), so a decoded-bit partition would put all 144 under bit 0.
    for (size_t index = 0; index < capture.size(); ++index) {
        if (ExpectedBit(pixels, index)) {
            capture[index].duration0 = 5;
        }
    }
    ws2812_pulse_stats_t stats;
    std::memset(&stats, 0x5A, sizeof(stats));
    REQUIRE(Aggregate(capture, kVectorB, pixels, stats));

    REQUIRE_FALSE(stats.match_available);
    REQUIRE(stats.match_count == 0);
    REQUIRE(stats.bit0_high_min_ns == 100);
    REQUIRE(stats.bit0_high_max_ns == 100);
    REQUIRE(stats.bit0_high_avg_ns == 100);
    REQUIRE(stats.bit1_high_min_ns == 125);   // from the 6 expected-1 symbols, not 0
    REQUIRE(stats.bit1_high_max_ns == 125);
    REQUIRE(stats.bit1_high_avg_ns == 125);
    REQUIRE(stats.bit0_first_high_ns == 100);
    REQUIRE(stats.bit0_first_low_ns == 700);
    REQUIRE(stats.bit1_first_high_ns == 125);   // symbol 10, the first expected 1
    REQUIRE(stats.bit1_first_low_ns == 700);
}

TEST_CASE("aggregate: vector B ideal capture, unclassifiable symbols are still excluded", "[T-14][FR-28][FR-32]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorB, pixels);
    REQUIRE(ExpectedBit(pixels, 10));
    capture[10] = Symbol(1000, 5, 0);   // the first expected-1 symbol is unclassifiable
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorB, pixels, stats));
    REQUIRE_FALSE(stats.match_available);
    REQUIRE(stats.match_count == 0);
    REQUIRE(stats.bit1_high_max_ns == 100);    // the 25,000 ns symbol is not in the stats
    REQUIRE(stats.bit1_first_high_ns == 100);  // first example is the next expected-1 symbol
}

TEST_CASE("aggregate: unequal highs report the match as available", "[T-14][FR-28]")
{
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    std::memset(&stats, 0, sizeof(stats));
    REQUIRE(Aggregate(IdealCapture(kVectorA, pixels), kVectorA, pixels, stats));
    REQUIRE(stats.match_available);
    REQUIRE(Aggregate(IdealCapture(kVectorD, pixels), kVectorD, pixels, stats));
    REQUIRE(stats.match_available);
}

TEST_CASE("aggregate: vector D (inverted highs) ideal capture reports 144/144", "[T-14][FR-27][FR-28]")
{
    // SPEC-003 vector D: bit0_high (1,200) > bit1_high (1,000). The nearest-high-time rule decodes
    // each transmitted bit correctly, so the stats are filed under the right bit.
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(IdealCapture(kVectorD, pixels), kVectorD, pixels, stats));
    REQUIRE(stats.match_available);
    REQUIRE(stats.match_count == 144);
    REQUIRE(stats.bit0_high_min_ns == 1200);
    REQUIRE(stats.bit0_high_max_ns == 1200);
    REQUIRE(stats.bit0_high_avg_ns == 1200);
    REQUIRE(stats.bit1_high_min_ns == 1000);
    REQUIRE(stats.bit1_high_max_ns == 1000);
    REQUIRE(stats.bit1_high_avg_ns == 1000);
    REQUIRE(stats.bit0_first_high_ns == 1200);
    REQUIRE(stats.bit0_first_low_ns == 800);
    REQUIRE(stats.bit1_first_high_ns == 1000);
    REQUIRE(stats.bit1_first_low_ns == 1000);
}

TEST_CASE("aggregate: vector D with a flipped bit counts one mismatch", "[T-14][FR-27][FR-28]")
{
    const Pixels pixels = FixedPattern();
    Capture capture = IdealCapture(kVectorD, pixels);
    REQUIRE_FALSE(ExpectedBit(pixels, 0));
    capture[0] = Symbol(40, 40);   // expected 0, received the (shorter) bit-1 shape
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(capture, kVectorD, pixels, stats));
    REQUIRE(stats.match_count == 143);
}

TEST_CASE("the pure functions touch no FreeRTOS, RMT or log fake", "[T-13][T-14][NFR-14]")
{
    ResetFakes();
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    Capture capture = IdealCapture(kVectorA, pixels);
    capture[3] = Symbol(1, 1, 0);
    REQUIRE(Aggregate(capture, kVectorA, pixels, stats));
    (void)DecodeWs2812Symbol(capture[0], 400, 800, kTickNs);
    (void)Ws2812TicksToNs(16, kTickNs);
    REQUIRE(fff.call_history_idx == 0);
}

// ---- T-13 / T-14 (2026-10-03): the tuner-reachable equal (AC) and inverted (AD) cases ------------------------------

TEST_CASE("DecodeWs2812Symbol: vector AD (600/500 ns) with the 550 ns tie as bit 0", "[T-13][FR-27]")
{
    REQUIRE(DecodeWs2812Symbol(Symbol(24, 56), 600, 500, kTickNs) == WS2812_SYMBOL_BIT0);   // 600 ns
    REQUIRE(DecodeWs2812Symbol(Symbol(20, 20), 600, 500, kTickNs) == WS2812_SYMBOL_BIT1);   // 500 ns
    REQUIRE(DecodeWs2812Symbol(Symbol(550, 0), 600, 500, 1) == WS2812_SYMBOL_BIT0);         // tie
    REQUIRE(DecodeWs2812Symbol(Symbol(549, 0), 600, 500, 1) == WS2812_SYMBOL_BIT1);
}

TEST_CASE("DecodeWs2812Symbol: vector AC (equal 500 ns highs) classifies every symbol as bit 0", "[T-13][FR-27]")
{
    for (uint32_t high_ticks : {0u, 19u, 20u, 21u, 40u}) {
        REQUIRE(DecodeWs2812Symbol(Symbol(high_ticks, 20), 500, 500, kTickNs) == WS2812_SYMBOL_BIT0);
    }
}

TEST_CASE("aggregate: vector AC ideal capture reports match n/a with stats by expected bit", "[T-14][FR-27][FR-28]")
{
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(IdealCapture(kVectorAC, pixels), kVectorAC, pixels, stats));
    REQUIRE_FALSE(stats.match_available);
    REQUIRE(stats.match_count == 0);
    REQUIRE(stats.bit0_high_avg_ns == 500);
    REQUIRE(stats.bit1_high_avg_ns == 500);
    REQUIRE(stats.bit0_first_low_ns == 750);   // bit-0 period 1,250
    REQUIRE(stats.bit1_first_low_ns == 500);   // bit-1 period 1,000: the expected-bit partition shows it
}

TEST_CASE("aggregate: vector AD ideal capture reports 144/144 with the inverted highs filed correctly", "[T-14][FR-27][FR-28]")
{
    const Pixels pixels = FixedPattern();
    ws2812_pulse_stats_t stats;
    REQUIRE(Aggregate(IdealCapture(kVectorAD, pixels), kVectorAD, pixels, stats));
    REQUIRE(stats.match_available);
    REQUIRE(stats.match_count == 144);
    REQUIRE(stats.bit0_high_avg_ns == 600);
    REQUIRE(stats.bit1_high_avg_ns == 500);
    REQUIRE(stats.bit0_first_low_ns == 1400);
    REQUIRE(stats.bit1_first_low_ns == 500);
}
