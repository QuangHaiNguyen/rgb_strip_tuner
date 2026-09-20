/**
 * @file test_button.cpp
 * @brief Host tests for the GPIO9 provisioning button (SPEC-002 T-1; FR-1, FR-2, FR-24, NFR-4).
 */
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

extern "C" {
#include "button.h"
#include "freertos_mock.h"
#include "gpio_fakes.h"
#include "host_stubs.h"
}

namespace {

std::vector<uint32_t> g_request_times_ms;

void OnRequest()
{
    g_request_times_ms.push_back(MockGetNowMs());
}

/** Boot the button service with the pin released and start its task. */
void StartService()
{
    MockFreeRtosReset();
    TestGpioReset();
    TestLogReset();
    g_request_times_ms.clear();
    REQUIRE(StartButton(OnRequest));
}

void RunTo(uint32_t until_ms)
{
    MockRunTask(0, until_ms);
}

int CountOccurrences(const std::string &text, const std::string &needle)
{
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("sample processing: press start, hold threshold, release", "[T-1][FR-1]")
{
    button_state_t state = {};

    REQUIRE(ProcessButtonSample(&state, false, 0, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, 100, 1000) == BUTTON_EVENT_PRESS_STARTED);
    REQUIRE(ProcessButtonSample(&state, true, 500, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, 1099, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, 1100, 1000) == BUTTON_EVENT_HOLD_REACHED);
    // The request is issued once per press.
    REQUIRE(ProcessButtonSample(&state, true, 1110, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, 9000, 1000) == BUTTON_EVENT_NONE);
}

TEST_CASE("a release before the threshold resets the hold timer", "[T-1][FR-1]")
{
    button_state_t state = {};
    ProcessButtonSample(&state, true, 0, 1000);
    REQUIRE(ProcessButtonSample(&state, true, 990, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, false, 1000, 1000) == BUTTON_EVENT_NONE);

    REQUIRE(ProcessButtonSample(&state, true, 1010, 1000) == BUTTON_EVENT_PRESS_STARTED);
    // 1,000 ms after the FIRST press would be 1,000; the timer restarted at 1,010.
    REQUIRE(ProcessButtonSample(&state, true, 1500, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, 2010, 1000) == BUTTON_EVENT_HOLD_REACHED);
}

TEST_CASE("a new press after a completed hold requests again", "[T-1][FR-3]")
{
    button_state_t state = {};
    ProcessButtonSample(&state, true, 0, 1000);
    REQUIRE(ProcessButtonSample(&state, true, 1000, 1000) == BUTTON_EVENT_HOLD_REACHED);
    ProcessButtonSample(&state, false, 1100, 1000);
    REQUIRE(ProcessButtonSample(&state, true, 2000, 1000) == BUTTON_EVENT_PRESS_STARTED);
    REQUIRE(ProcessButtonSample(&state, true, 3000, 1000) == BUTTON_EVENT_HOLD_REACHED);
}

TEST_CASE("hold timing survives the 32-bit millisecond wrap", "[T-1][FR-1]")
{
    button_state_t state = {};
    const uint32_t start_ms = 0xFFFFFF00u;
    ProcessButtonSample(&state, true, start_ms, 1000);
    REQUIRE(ProcessButtonSample(&state, true, start_ms + 999, 1000) == BUTTON_EVENT_NONE);
    REQUIRE(ProcessButtonSample(&state, true, start_ms + 1000, 1000) == BUTTON_EVENT_HOLD_REACHED);
}

TEST_CASE("GPIO9 is configured as a pulled-up input", "[T-1][FR-2][S5]")
{
    MockFreeRtosReset();
    TestGpioReset();
    REQUIRE(StartButton(OnRequest));

    const gpio_config_t *config = TestGpioLastConfig();
    REQUIRE(TestGpioConfigCalls() == 1);
    REQUIRE(config->pin_bit_mask == (1ULL << 9));
    REQUIRE(config->mode == GPIO_MODE_INPUT);   // never an output
    REQUIRE(config->pull_up_en == GPIO_PULLUP_ENABLE);
    REQUIRE(config->intr_type == GPIO_INTR_DISABLE);
    REQUIRE(MockGetTaskCount() == 1);
}

TEST_CASE("a GPIO configuration failure does not start the task", "[T-1][FR-2]")
{
    MockFreeRtosReset();
    TestGpioReset();
    TestGpioFailConfig(true);
    REQUIRE_FALSE(StartButton(OnRequest));
    REQUIRE(MockGetTaskCount() == 0);
}

TEST_CASE("the task samples GPIO9 at least every 10 ms", "[T-1][FR-1][NFR-4]")
{
    StartService();
    RunTo(2000);

    REQUIRE(TestGpioLastReadPin() == 9);
    REQUIRE(TestGpioReadCount() >= 200);
    for (int index = 1; index < TestGpioReadCount(); ++index) {
        REQUIRE(TestGpioReadTimeMs(index) - TestGpioReadTimeMs(index - 1) <= 10);
    }
}

TEST_CASE("a 1,000 ms hold requests provisioning within 1,050 ms of press start", "[T-1][FR-1][FR-24][NFR-4]")
{
    StartService();
    RunTo(500);
    TestGpioSetLevel(0);   // press begins at t = 500 ms, after the firmware is running
    RunTo(500 + 999);
    REQUIRE(g_request_times_ms.empty());

    RunTo(500 + 1100);
    REQUIRE(g_request_times_ms.size() == 1);
    REQUIRE(g_request_times_ms[0] >= 500 + 1000);
    REQUIRE(g_request_times_ms[0] <= 500 + 1050);
}

TEST_CASE("a press that is already down when the service starts still counts", "[T-1][FR-3]")
{
    MockFreeRtosReset();
    TestGpioReset();
    TestLogReset();
    g_request_times_ms.clear();
    TestGpioSetLevel(0);
    REQUIRE(StartButton(OnRequest));
    RunTo(1200);
    REQUIRE(g_request_times_ms.size() == 1);
    REQUIRE(g_request_times_ms[0] <= 1050);
}

TEST_CASE("a press shorter than 1,000 ms does not request provisioning", "[T-1][FR-1]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(990);
    TestGpioSetLevel(1);
    RunTo(3000);
    REQUIRE(g_request_times_ms.empty());
}

TEST_CASE("holding longer requests once; releasing and pressing again requests again", "[T-1][FR-1][FR-3]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(5000);
    REQUIRE(g_request_times_ms.size() == 1);

    TestGpioSetLevel(1);
    RunTo(5100);
    TestGpioSetLevel(0);
    RunTo(7000);
    REQUIRE(g_request_times_ms.size() == 2);
}

TEST_CASE("a bouncing release resets the timer", "[T-1][FR-1]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(900);
    TestGpioSetLevel(1);
    RunTo(920);
    TestGpioSetLevel(0);
    RunTo(1800);          // 880 ms since the bounce: below the threshold
    REQUIRE(g_request_times_ms.empty());
    RunTo(2000);          // 1,080 ms since the bounce
    REQUIRE(g_request_times_ms.size() == 1);
}

TEST_CASE("the press-start and threshold cues are logged at Info", "[T-1][FR-24]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(3000);

    const std::string log = TestLogText();
    REQUIRE(CountOccurrences(log, "[L1 button] button press started") == 1);
    REQUIRE(CountOccurrences(log, "[L1 button] button held 1000 ms, provisioning requested") == 1);
}

TEST_CASE("a short press logs only the press-start cue", "[T-1][FR-24]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(300);
    TestGpioSetLevel(1);
    RunTo(2000);

    const std::string log = TestLogText();
    REQUIRE(CountOccurrences(log, "button press started") == 1);
    REQUIRE(CountOccurrences(log, "provisioning requested") == 0);
}

TEST_CASE("the service keeps running after a request", "[T-1][FR-1]")
{
    StartService();
    TestGpioSetLevel(0);
    RunTo(1500);
    const int reads_before = TestGpioReadCount();
    RunTo(4000);
    REQUIRE(TestGpioReadCount() > reads_before + 200);
}
