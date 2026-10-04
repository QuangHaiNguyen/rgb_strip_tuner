/**
 * @file test_led_publish_integration.cpp
 * @brief SPEC-004 T-19/T-20 across both components (FR-16, FR-24, FR-33, FR-36): led_controller.c and the real
 *        rmt_pulse_monitor.c linked together (RMT and FreeRTOS are FFF fakes), with a result callback registered.
 *
 * Checks the owner decision of 2026-10-03 end to end: a frame led_controller skips before calling ArmPulseCapture()
 * (previous frame still in flight, encoder update failure) publishes no result, while the monitor-detected cases
 * (monitor not started, RX lock busy, arm failed) publish not_measured for the frame's submit_seq.
 */
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "fff.h"
extern "C" {
#include "freertos_fakes.h"
#include "led_controller.h"
#include "led_controller_harness.h"
#include "log_fakes.h"
#include "rmt_fakes.h"
#include "rmt_pulse_monitor.h"
#include "rmt_pulse_monitor_harness.h"
}

namespace {

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};
const ws2812_timing_t kVectorX = {1000, 2000, 1200, 2000, 800};

std::vector<ws2812_measurement_t> g_published;
void RecordPublished(const ws2812_measurement_t *measurement) { g_published.push_back(*measurement); }

void ResetFakes()
{
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
}

/** Start both components with every fake succeeding (as app_main() does: monitor first), then register the callback. */
void StartBoth(bool monitor_ok = true)
{
    ResetFakes();
    HarnessResetPulseMonitor();
    HarnessResetLedController();
    if (!monitor_ok) {
        rmt_new_rx_channel_fake.return_val = ESP_ERR_NOT_FOUND;
    }
    REQUIRE(StartPulseMonitor() == monitor_ok);
    RmtFakesReset();
    REQUIRE(StartLedController());
    ResetFakes();
    g_published.clear();
    SetPulseResultCallback(RecordPublished);
}

}  // namespace

TEST_CASE("integration: a transmitted frame arms with its submit_seq and publishes nothing from the arm path", "[T-19][T-20][FR-24][FR-33]")
{
    StartBoth();
    const led_request_t request = {kVectorA, 5};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(HarnessGetArmedSubmitSeq() == 5);
    REQUIRE(g_published.empty());   // its outcome comes later from the decode task
}

TEST_CASE("integration: a frame skipped because the previous one is in flight publishes no result", "[T-19][FR-16][FR-33][FR-36]")
{
    StartBoth();
    rmt_tx_wait_all_done_fake.return_val = ESP_ERR_TIMEOUT;
    const led_request_t requests[] = {{kVectorA, 11}, {kVectorX, 12}};
    HarnessRunDriverTaskRequests(requests, 2);
    REQUIRE(std::string(TestLogText()).find("previous frame still in flight, frame skipped") != std::string::npos);
    REQUIRE(rmt_receive_fake.call_count == 1);   // only frame 11 armed
    REQUIRE(HarnessGetArmedSubmitSeq() == 11);
    REQUIRE(g_published.empty());                // nothing for 12: the page times out (owner decision)
}

TEST_CASE("integration: a frame whose encoder update fails publishes no result", "[T-19][FR-16][FR-33][FR-36]")
{
    StartBoth();
    rmt_bytes_encoder_update_config_fake.return_val = ESP_FAIL;
    const led_request_t request = {kVectorX, 21};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(g_published.empty());
}

TEST_CASE("integration: with the monitor not started every frame publishes not_measured for its number", "[T-20][FR-20][FR-36]")
{
    StartBoth(false);
    const led_request_t requests[] = {{kVectorA, 31}, {kVectorX, 32}};
    HarnessRunDriverTaskRequests(requests, 2);
    REQUIRE(rmt_transmit_fake.call_count == 2);   // the strip is still driven (FR-20)
    REQUIRE(g_published.size() == 2);
    REQUIRE(g_published[0].submit_seq == 31);
    REQUIRE(g_published[0].state == WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(g_published[1].submit_seq == 32);
    REQUIRE(g_published[1].state == WS2812_MEASUREMENT_NOT_MEASURED);
}

TEST_CASE("integration: a busy RX lock or a failed arm publishes not_measured, and the frame is still sent", "[T-20][FR-24][FR-36]")
{
    StartBoth();
    SECTION("lock busy")
    {
        xSemaphoreTake_fake.return_val = pdFALSE;
    }
    SECTION("arm failed")
    {
        rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
    }
    const led_request_t request = {kVectorA, 41};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].submit_seq == 41);
    REQUIRE(g_published[0].state == WS2812_MEASUREMENT_NOT_MEASURED);
}

TEST_CASE("integration: no callback registered -> skipped and unarmed frames run without a crash", "[T-20][FR-37]")
{
    StartBoth(false);
    SetPulseResultCallback(nullptr);
    const led_request_t request = {kVectorA, 51};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(g_published.empty());
}
