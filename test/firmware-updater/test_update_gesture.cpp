/**
 * @file test_update_gesture.cpp
 * @brief SPEC-007 T-4 (FR-9, FR-31): the update gesture of main/button/button.c. Pure state machine
 *        (ProcessButtonSample() with 10 ms samples) and the sampling task on the fake FreeRTOS clock with GPIO9 faked:
 *        5 presses of 100 ms within 3 s give one event within 50 ms of the 5th release; 4 presses, a 6th press,
 *        presses spread over 3,010 ms, a 1,000 ms hold (provisioning, no gesture) and a 990 ms press in the sequence.
 *        The SPEC-002 hold detection itself is covered by rerunning test/captive-portal (button_tests).
 */
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "button.h"
#include "freertos_mock.h"
#include "gpio_fakes.h"
#include "host_stubs.h"
}

namespace {

/** One press: start time and duration, in ms (multiples of the 10 ms sample period). A press of D ms reads low at the
 *  samples start .. start + D, so a 1,000 ms press reaches the 1,000 ms hold threshold and a 990 ms press does not. */
using Press = std::pair<uint32_t, uint32_t>;

struct Outcome {
    std::vector<uint32_t> gesture_ms;
    std::vector<uint32_t> hold_ms;
};

/** Feed 10 ms samples from 0 to @p end_ms to ProcessButtonSample(). */
Outcome RunPure(const std::vector<Press> &presses, uint32_t end_ms, uint32_t start_ms = 0)
{
    button_state_t state = {};
    Outcome outcome;
    for (uint32_t now = start_ms; now - start_ms <= end_ms; now += BUTTON_SAMPLE_MS) {
        uint32_t t = now - start_ms;
        bool is_low = false;
        for (const Press &press : presses) {
            if (t >= press.first && t <= press.first + press.second) {
                is_low = true;
            }
        }
        switch (ProcessButtonSample(&state, is_low, now, BUTTON_HOLD_MS)) {
        case BUTTON_EVENT_UPDATE_GESTURE: outcome.gesture_ms.push_back(t); break;
        case BUTTON_EVENT_HOLD_REACHED: outcome.hold_ms.push_back(t); break;
        default: break;
        }
    }
    return outcome;
}

std::vector<Press> Evenly(int count, uint32_t period_ms, uint32_t duration_ms, uint32_t first_ms = 100)
{
    std::vector<Press> presses;
    for (int index = 0; index < count; ++index) {
        presses.push_back({first_ms + (uint32_t)index * period_ms, duration_ms});
    }
    return presses;
}

/* ---- task level ---- */
std::vector<uint32_t> g_update_ms;
std::vector<uint32_t> g_request_ms;
void OnUpdate() { g_update_ms.push_back(MockGetNowMs()); }
void OnRequest() { g_request_ms.push_back(MockGetNowMs()); }

/** Run the button task with GPIO9 driven by @p presses until @p end_ms. */
void RunTask(const std::vector<Press> &presses, uint32_t end_ms)
{
    MockFreeRtosReset();
    TestGpioReset();
    TestLogReset();
    g_update_ms.clear();
    g_request_ms.clear();
    REQUIRE(StartButton(OnRequest, OnUpdate));
    std::vector<std::pair<uint32_t, int>> edges;
    for (const Press &press : presses) {
        edges.push_back({press.first, 0});
        edges.push_back({press.first + press.second, 1});
    }
    for (const auto &edge : edges) {
        MockRunTask(0, edge.first);
        TestGpioSetLevel(edge.second);
    }
    MockRunTask(0, end_ms);
}

int Count(const std::string &text, const std::string &needle)
{
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("constants: 5 presses within 3,000 ms (NFR-10: >= 20 ms, multiple of 10 ms)", "[T-4][FR-9][NFR-10]")
{
    CHECK(FW_UPDATE_GESTURE_PRESSES == 5);
    CHECK(FW_UPDATE_GESTURE_WINDOW_MS == 3000);
    CHECK(FW_UPDATE_GESTURE_WINDOW_MS % 10 == 0);
}

TEST_CASE("5 presses of 100 ms within 3 s -> exactly one event within 50 ms of the 5th release", "[T-4][FR-9]")
{
    const std::vector<Press> presses = Evenly(5, 300, 100);   /* starts 100..1300, last release 1400 */
    Outcome outcome = RunPure(presses, 6000);
    REQUIRE(outcome.gesture_ms.size() == 1);
    const uint32_t last_release = presses.back().first + presses.back().second;   /* last low sample */
    CHECK(outcome.gesture_ms[0] > last_release);
    CHECK(outcome.gesture_ms[0] <= last_release + 50);
    CHECK(outcome.hold_ms.empty());
}

TEST_CASE("5 fast presses (50 ms on, 50 ms off) also give one event", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(5, 100, 50), 5000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("the 5 press starts exactly 3,000 ms apart still count (window inclusive)", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(5, 750, 100), 8000);   /* starts 100 .. 3100: span 3,000 ms */
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("4 presses -> no event", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(4, 300, 100), 10000);
    CHECK(outcome.gesture_ms.empty());
    CHECK(outcome.hold_ms.empty());
}

TEST_CASE("presses spread over 3,010 ms (1st to 5th start) -> no event", "[T-4][FR-9]")
{
    std::vector<Press> presses = {{0, 100}, {750, 100}, {1500, 100}, {2250, 100}, {3010, 100}};
    Outcome outcome = RunPure(presses, 10000, 1000);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("a 6th press after the gesture gives no second event", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(6, 300, 100), 10000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("10 presses within 3 s give at most 2 events, never one per press after the 5th", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(10, 250, 100), 10000);
    CHECK(outcome.gesture_ms.size() <= 2);
}

TEST_CASE("a single 1,000 ms press is a provisioning hold and no gesture", "[T-4][FR-9][FR-31]")
{
    Outcome outcome = RunPure({{100, 1000}}, 6000);
    CHECK(outcome.hold_ms.size() == 1);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("4 short presses then a 1,000 ms 5th press: provisioning hold only, the count is reset", "[T-4][FR-9][FR-31]")
{
    std::vector<Press> presses = Evenly(4, 200, 100);
    presses.push_back({1000, 1100});
    Outcome outcome = RunPure(presses, 8000);
    CHECK(outcome.hold_ms.size() == 1);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("a hold in the middle resets the count: 2 short + hold + 3 short -> no event", "[T-4][FR-9]")
{
    std::vector<Press> presses = {{0, 100}, {200, 100}, {400, 1000}, {1500, 100}, {1700, 100}, {1900, 100}};
    Outcome outcome = RunPure(presses, 8000);
    CHECK(outcome.hold_ms.size() == 1);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("a press of 990 ms in the sequence still counts (each press < 1,000 ms)", "[T-4][FR-9]")
{
    std::vector<Press> presses = {{0, 100}, {200, 990}, {1300, 100}, {1500, 100}, {1700, 100}};
    Outcome outcome = RunPure(presses, 8000);
    CHECK(outcome.hold_ms.empty());
    REQUIRE(outcome.gesture_ms.size() == 1);
    CHECK(outcome.gesture_ms[0] <= 1800 + 50);
}

TEST_CASE("a release followed by no press within 3 s resets the count", "[T-4][FR-9]")
{
    /* 3 presses, a pause longer than the window, then 2 presses: never 5 within the window. */
    std::vector<Press> presses = {{0, 100}, {200, 100}, {400, 100}, {3600, 100}, {3800, 100}};
    Outcome outcome = RunPure(presses, 10000);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("a new gesture after a stale partial one is still recognized", "[T-4][FR-9]")
{
    std::vector<Press> presses = {{0, 100}, {200, 100}};
    for (int index = 0; index < 5; ++index) {
        presses.push_back({5000 + (uint32_t)index * 200, 100});
    }
    Outcome outcome = RunPure(presses, 10000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("the gesture survives the 32-bit millisecond wrap", "[T-4][FR-9]")
{
    Outcome outcome = RunPure(Evenly(5, 300, 100), 6000, 0xFFFFFF00u);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("task: 5 presses of 100 ms -> on_update once within 50 ms of the 5th release, no provisioning request",
          "[T-4][FR-9][FR-31]")
{
    const std::vector<Press> presses = Evenly(5, 300, 100);
    RunTask(presses, 8000);
    REQUIRE(g_update_ms.size() == 1);
    const uint32_t last_release = presses.back().first + presses.back().second;
    CHECK(g_update_ms[0] >= last_release);
    CHECK(g_update_ms[0] <= last_release + 50);
    CHECK(g_request_ms.empty());
    const std::string log = TestLogText();
    CHECK(Count(log, "[L1 button] button pressed 5 times, update requested") == 1);
    CHECK(Count(log, "[L1 button] button press started") == 5);   /* SPEC-002 FR-24 cue kept for each press */
}

TEST_CASE("task: a 1,000 ms hold -> provisioning request, no update", "[T-4][FR-31]")
{
    RunTask({{100, 1200}}, 5000);
    CHECK(g_request_ms.size() == 1);
    CHECK(g_update_ms.empty());
}

TEST_CASE("task: 4 presses and presses spread over 3,010 ms -> no update", "[T-4][FR-9]")
{
    SECTION("4 presses") { RunTask(Evenly(4, 300, 100), 8000); }
    SECTION("spread over 3,010 ms") { RunTask({{100, 100}, {850, 100}, {1600, 100}, {2350, 100}, {3110, 100}}, 9000); }
    CHECK(g_update_ms.empty());
    CHECK(g_request_ms.empty());
}

TEST_CASE("task: a NULL on_update callback is allowed (gesture logged, nothing called)", "[T-4][FR-9]")
{
    MockFreeRtosReset();
    TestGpioReset();
    TestLogReset();
    g_request_ms.clear();
    REQUIRE(StartButton(OnRequest, nullptr));
    for (const Press &press : Evenly(5, 300, 100)) {
        MockRunTask(0, press.first);
        TestGpioSetLevel(0);
        MockRunTask(0, press.first + press.second);
        TestGpioSetLevel(1);
    }
    MockRunTask(0, 5000);
    CHECK(Count(TestLogText(), "update requested") == 1);
    CHECK(g_request_ms.empty());
}

/* ---- T-19 / FR-9 (section 0.7): sliding window over the last 5 press starts ----------------------------------------- */

namespace {

std::vector<Press> Starts(std::initializer_list<uint32_t> starts, uint32_t duration_ms = 100)
{
    std::vector<Press> presses;
    for (uint32_t start : starts) {
        presses.push_back({start, duration_ms});
    }
    return presses;
}

}  // namespace

TEST_CASE("T-19 vector: a stray press at 0 ms, then 5 x 100 ms presses at 2,500..3,700 ms -> exactly one event",
          "[T-19][T-4][FR-9]")
{
    const std::vector<Press> presses = Starts({0, 2500, 2800, 3100, 3400, 3700});
    Outcome outcome = RunPure(presses, 10000);
    REQUIRE(outcome.gesture_ms.size() == 1);
    CHECK(outcome.gesture_ms[0] > 3800);
    CHECK(outcome.gesture_ms[0] <= 3800 + 50);
    CHECK(outcome.hold_ms.empty());
}

TEST_CASE("T-19 vector through the button task: on_update once within 50 ms of the last release", "[T-19][T-4][FR-9]")
{
    RunTask(Starts({0, 2500, 2800, 3100, 3400, 3700}), 10000);
    REQUIRE(g_update_ms.size() == 1);
    CHECK(g_update_ms[0] >= 3800);
    CHECK(g_update_ms[0] <= 3800 + 50);
    CHECK(g_request_ms.empty());
}

TEST_CASE("7 presses where only the last 5 fit the window -> one event, on the 7th release", "[T-19][FR-9]")
{
    Outcome outcome = RunPure(Starts({0, 400, 3200, 3400, 3600, 3800, 4000}), 10000);
    REQUIRE(outcome.gesture_ms.size() == 1);
    CHECK(outcome.gesture_ms[0] > 4100);
    CHECK(outcome.gesture_ms[0] <= 4100 + 50);
}

TEST_CASE("6 presses where the last 5 fit only after the first slides out -> one event, on the 6th release",
          "[T-19][FR-9]")
{
    Outcome outcome = RunPure(Starts({0, 1500, 3100, 3300, 3500, 3700}), 10000);
    REQUIRE(outcome.gesture_ms.size() == 1);
    CHECK(outcome.gesture_ms[0] > 3800);
    CHECK(outcome.gesture_ms[0] <= 3850);
}

TEST_CASE("two stray presses before a valid sequence still give exactly one event", "[T-19][FR-9]")
{
    Outcome outcome = RunPure(Starts({0, 1200, 3000, 3200, 3400, 3600, 3800}), 10000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("sliding: no 5 consecutive starts within 3,000 ms -> no event (evenly every 760 ms, 10 presses)",
          "[T-19][FR-9]")
{
    /* Any 5 consecutive starts span 4 x 760 = 3,040 ms. */
    Outcome outcome = RunPure(Evenly(10, 760, 100), 15000);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("sliding: starts every 750 ms -> 5 consecutive starts span exactly 3,000 ms -> one event at the 5th",
          "[T-19][FR-9]")
{
    Outcome outcome = RunPure(Evenly(5, 750, 100), 10000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("sliding: a recognized gesture resets the ring - presses 6 to 9 give no second event", "[T-19][FR-9]")
{
    Outcome outcome = RunPure(Evenly(9, 200, 100), 10000);
    CHECK(outcome.gesture_ms.size() == 1);
}

TEST_CASE("sliding: a hold resets the ring - 4 short, hold, 4 short -> no event; then a 5th short -> one event",
          "[T-19][FR-9][FR-31]")
{
    std::vector<Press> presses = Starts({0, 200, 400, 600});
    presses.push_back({800, 1100});
    for (uint32_t start : {2000u, 2200u, 2400u, 2600u}) {
        presses.push_back({start, 100});
    }
    Outcome without_fifth = RunPure(presses, 10000);
    CHECK(without_fifth.hold_ms.size() == 1);
    CHECK(without_fifth.gesture_ms.empty());

    presses.push_back({2800, 100});
    Outcome with_fifth = RunPure(presses, 10000);
    CHECK(with_fifth.hold_ms.size() == 1);
    CHECK(with_fifth.gesture_ms.size() == 1);
}

TEST_CASE("sliding: the 5th press held to 1,000 ms is a provisioning hold, no gesture on its release",
          "[T-19][FR-9][FR-31]")
{
    std::vector<Press> presses = Starts({0, 200, 400, 600});
    presses.push_back({800, 1000});
    Outcome outcome = RunPure(presses, 10000);
    CHECK(outcome.hold_ms.size() == 1);
    CHECK(outcome.gesture_ms.empty());
}

TEST_CASE("sliding: a 990 ms press inside a sliding sequence still counts", "[T-19][FR-9]")
{
    std::vector<Press> presses = Starts({0, 2400});
    presses.push_back({2600, 990});
    for (uint32_t start : {3700u, 3900u, 4100u}) {
        presses.push_back({start, 100});
    }
    /* Last 5 starts: 2400, 2600, 3700, 3900, 4100 -> span 1,700 ms. */
    Outcome outcome = RunPure(presses, 10000);
    CHECK(outcome.hold_ms.empty());
    CHECK(outcome.gesture_ms.size() == 1);
}
