/**
 * @file test_read_overlap.cpp
 * @brief SPEC-006 FR-13 / use case 8.3 (host part of T-5 and T-8): the overlap-race fix in rmt_pulse_monitor.c
 *        (2026-10-04, `s_is_receive_pending`).
 *
 * Before the fix, an arm made while a receive was in flight retagged s_capture_seq, called rmt_receive() (which failed
 * busy) and restored the tag. A done ISR inside that window tagged the pending capture with the failed arm's number,
 * so the decode task discarded it as stale and the pending capture timed out. Now an arm that finds a receive in
 * flight fails at once: no rmt_receive(), no retagging. These cases check that, the race itself, that the flag is
 * always cleared (done ISR, timeout restart, failed rmt_receive) and that non-overlapping flows are unchanged.
 *
 * rmt_pulse_monitor.c is compiled through mocks/rmt_pulse_monitor_read_harness.c; RMT and FreeRTOS are FFF fakes.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "fff.h"
extern "C" {
#include "freertos_fakes.h"
#include "log_fake_access.h"
#include "log_fakes.h"
#include "rmt_fakes.h"
#include "rmt_pulse_monitor.h"
#include "rmt_pulse_monitor_read_harness.h"
}

namespace {

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};
const uint8_t kPattern[18] = {
    0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x20,
};
const char *const kArmFailed = "pulse monitor: arm failed (err=259)";
const char *const kR1Line = "pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; bits=143";
const std::vector<std::string> kSendLines = {
    "pulse: bit0 first high_ns=400 low_ns=850; bit1 first high_ns=800 low_ns=450",
    "pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800",
    "pulse: grb match=144/144",
};

enum class Mode { Send, Read };
const char *Name(Mode mode) { return mode == Mode::Send ? "send" : "read"; }

harness_capture_t Arrive(size_t symbol_count, uint32_t stale_by = 0, void (*before_delivery)() = nullptr)
{
    return harness_capture_t{true, symbol_count, stale_by, before_delivery};
}
harness_capture_t Timeout(void (*during_wait)() = nullptr) { return harness_capture_t{false, 0, 0, during_wait}; }

void ResetFakes()
{
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
}

void StartMonitor()
{
    ResetFakes();
    HarnessResetReadPulseMonitor();
    SetPulseResultCallback(nullptr);
    REQUIRE(StartPulseMonitor());
    REQUIRE_FALSE(HarnessIsReceivePending());
    ResetFakes();
}

void Arm(Mode mode, uint32_t submit_seq)
{
    if (mode == Mode::Send) {
        ArmPulseCapture(&kVectorA, submit_seq, kPattern, sizeof(kPattern));
    } else {
        ArmPulseRead(submit_seq);
    }
}

/** Start the monitor and arm one receive of @p mode successfully; the fakes are reset afterwards. */
void StartAndArm(Mode mode, uint32_t submit_seq)
{
    StartMonitor();
    Arm(mode, submit_seq);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(HarnessIsReceivePending());
    ResetFakes();
}

rmt_symbol_word_t Sym(uint32_t d0, uint32_t d1)
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = d0;
    symbol.level0 = 1;
    symbol.duration1 = d1;
    symbol.level1 = 0;
    return symbol;
}

void FillR1()
{
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < 143; ++index) {
        buffer[index] = index % 2 == 0 ? Sym(16, 34) : Sym(32, 18);
    }
    buffer[143] = Sym(16, 0);
}

/** Ideal capture of the fixed pattern at vector A (as test/led-controller's FillIdealCapture()). */
void FillIdealSend()
{
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < RMT_PULSE_MONITOR_DATA_SYMBOLS; ++index) {
        const bool bit = ((kPattern[index / 8] >> (7 - index % 8)) & 1) != 0;
        buffer[index] = bit ? Sym(32, 18) : Sym(16, 34);
    }
    buffer[RMT_PULSE_MONITOR_DATA_SYMBOLS - 1].duration1 = 0;
}

std::string Log() { return TestLogText(); }

std::vector<std::string> LinesAt(int level)
{
    std::vector<std::string> lines;
    std::istringstream stream(Log());
    const std::string prefix = "[L" + std::to_string(level) + " pulse_mon] ";
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(prefix, 0) == 0) {
            lines.push_back(line.substr(prefix.size()));
        }
    }
    return lines;
}

int RxLockBalance()
{
    int balance = 0;
    for (unsigned index = 0; index < xSemaphoreTake_fake.call_count && index < FFF_ARG_HISTORY_LEN; ++index) {
        balance += (xSemaphoreTake_fake.arg0_history[index] == HarnessGetRxLock());
    }
    for (unsigned index = 0; index < xSemaphoreGive_fake.call_count && index < FFF_ARG_HISTORY_LEN; ++index) {
        balance -= (xSemaphoreGive_fake.arg0_history[index] == HarnessGetRxLock());
    }
    return balance;
}

struct Published {
    ws2812_measurement_t measurement;
    int rx_lock_balance;
    int warnings_before;
};
std::vector<Published> g_published;

void RecordPublished(const ws2812_measurement_t *measurement)
{
    g_published.push_back({*measurement, RxLockBalance(), TestLogCount(LOG_LEVEL_WARNING)});
}

void ListenForResults()
{
    g_published.clear();
    SetPulseResultCallback(RecordPublished);
}

// Done events the ISR handed to xQueueOverwriteFromISR().
std::vector<uint32_t> g_isr_tags;
BaseType_t RecordIsrEvent(QueueHandle_t, const void *item, BaseType_t *woken)
{
    size_t count = 0;
    uint32_t tag = 0;
    HarnessReadCaptureEvent(item, &count, &tag);
    g_isr_tags.push_back(tag);
    *woken = pdFALSE;
    return pdTRUE;
}

/** The RMT done ISR of the in-flight receive (a full 144-symbol frame), recording the tag it posts. */
void FireDoneIsr()
{
    xQueueOverwriteFromISR_fake.custom_fake = RecordIsrEvent;
    HarnessInvokeRxDone(144);
}

/** Second arm while a receive is in flight: no driver call, tag kept, one Warning, not_measured after the lock. */
void RequireBusyArmRejected(uint32_t submit_seq, uint32_t capture_seq_before, uint32_t armed_seq_before)
{
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 0);           // no re-enable attempt either
    REQUIRE(HarnessGetCaptureSeq() == capture_seq_before);
    REQUIRE(HarnessGetArmedSeq() == armed_seq_before);
    REQUIRE(HarnessIsReceivePending());                 // the in-flight receive still owns the channel
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{kArmFailed});
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == submit_seq);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(g_published[0].rx_lock_balance == 0);       // published after the lock was released
    REQUIRE(g_published[0].warnings_before == 1);       // after the Warning
}

}  // namespace

// ==== 1. A receive in flight: a second arm of either mode is rejected before rmt_receive() ===========================

TEST_CASE("overlap: a second arm while a receive is in flight makes no rmt_receive call and keeps the tag", "[T-5][T-8][FR-13][UC-8.3]")
{
    for (Mode first : {Mode::Read, Mode::Send}) {
        for (Mode second : {Mode::Read, Mode::Send}) {
            DYNAMIC_SECTION(Name(first) << " in flight, then a " << Name(second) << " arm")
            {
                StartAndArm(first, 10);
                ListenForResults();
                const uint32_t capture_seq = HarnessGetCaptureSeq();
                const uint32_t armed_seq = HarnessGetArmedSeq();
                REQUIRE(capture_seq == 1);

                Arm(second, 11);
                RequireBusyArmRejected(11, capture_seq, armed_seq);
                REQUIRE(HarnessIsArmedModeRead() == (first == Mode::Read));   // snapshot of the first arm kept
                REQUIRE(HarnessGetArmedSubmitSeq() == 10);
                REQUIRE(rmt_transmit_fake.call_count == 0);
            }
        }
    }
}

TEST_CASE("overlap: repeated arms while busy each fail the same way; nothing is queued or retried", "[T-5][FR-13]")
{
    StartAndArm(Mode::Read, 20);
    ListenForResults();
    Arm(Mode::Send, 21);
    Arm(Mode::Read, 22);
    Arm(Mode::Send, 23);
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>(3, kArmFailed));
    REQUIRE(g_published.size() == 3);
    for (size_t index = 0; index < 3; ++index) {
        REQUIRE(g_published[index].measurement.submit_seq == 21 + index);
        REQUIRE(g_published[index].measurement.state == WS2812_MEASUREMENT_NOT_MEASURED);
    }
    REQUIRE(HarnessGetCaptureSeq() == 1);
    REQUIRE(HarnessGetArmedSubmitSeq() == 20);

    // Once the read's receive ends, the next arm goes ahead (nothing was queued meanwhile).
    FireDoneIsr();
    REQUIRE_FALSE(HarnessIsReceivePending());
    Arm(Mode::Send, 24);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(HarnessGetArmedSeq() == 2);
}

TEST_CASE("overlap: the busy path is independent of the driver state (no rmt_receive even if it would succeed)", "[T-5][FR-13]")
{
    StartAndArm(Mode::Send, 30);
    ListenForResults();
    // rmt_receive() and rmt_enable() succeed by default: a pre-fix arm would have started a second receive here.
    Arm(Mode::Read, 31);
    RequireBusyArmRejected(31, 1, 1);
}

// ==== 2. The race: the done ISR around a failed arm carries the pending arm's tag ====================================

namespace {
uint32_t g_capture_seq_at_isr = 0;
void FireDoneIsrAtLockRelease()
{
    g_capture_seq_at_isr = HarnessGetCaptureSeq();
    FireDoneIsr();
}
}  // namespace

TEST_CASE("race: Read N pending, a Send arm fails, the done ISR fires -> read_done for N, no stale, no timeout", "[T-5][T-8][FR-13][UC-8.3]")
{
    SECTION("ISR at the failed Send arm's lock release")
    {
        StartAndArm(Mode::Read, 40);
        ListenForResults();
        g_isr_tags.clear();
        HarnessSetAfterRxLockReleaseHook(FireDoneIsrAtLockRelease, 0);
        Arm(Mode::Send, 41);
        HarnessSetAfterRxLockReleaseHook(nullptr, 0);
        REQUIRE(g_capture_seq_at_isr == 1);   // the tag the ISR saw inside the failed arm: Read N's
    }
    SECTION("ISR right after the failed Send arm returned")
    {
        StartAndArm(Mode::Read, 40);
        ListenForResults();
        g_isr_tags.clear();
        Arm(Mode::Send, 41);
        FireDoneIsr();
    }
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(g_isr_tags == std::vector<uint32_t>{1});   // tag N = arm 1 (the read), never the failed arm's
    REQUIRE_FALSE(HarnessIsReceivePending());
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 41);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_NOT_MEASURED);

    // The decode task receives that event (tagged like the ISR: the current s_capture_seq, unchanged).
    ResetFakes();
    g_published.clear();
    FillR1();
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);
    REQUIRE(HarnessCaptureWaitTicks(0) == 100);          // the read's bound
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG).empty());           // no "stale capture discarded"
    REQUIRE(LinesAt(LOG_LEVEL_WARNING).empty());         // no timeout Warning
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{kR1Line});
    REQUIRE(rmt_disable_fake.call_count == 0);           // no restart
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 40);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 400);
    REQUIRE(g_published[0].measurement.bit1_period_avg_ns == 1250);
}

TEST_CASE("race mirror: Send N pending, a Read arm fails, the done ISR fires -> done for the Send, no stale, no timeout", "[T-5][T-8][FR-13][UC-8.3]")
{
    SECTION("ISR at the failed Read arm's lock release")
    {
        StartAndArm(Mode::Send, 50);
        ListenForResults();
        g_isr_tags.clear();
        HarnessSetAfterRxLockReleaseHook(FireDoneIsrAtLockRelease, 0);
        Arm(Mode::Read, 51);
        HarnessSetAfterRxLockReleaseHook(nullptr, 0);
        REQUIRE(g_capture_seq_at_isr == 1);
    }
    SECTION("ISR right after the failed Read arm returned")
    {
        StartAndArm(Mode::Send, 50);
        ListenForResults();
        g_isr_tags.clear();
        Arm(Mode::Read, 51);
        FireDoneIsr();
    }
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(g_isr_tags == std::vector<uint32_t>{1});
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 51);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_NOT_MEASURED);

    ResetFakes();
    g_published.clear();
    FillIdealSend();
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);
    REQUIRE(HarnessCaptureWaitTicks(0) == 2);            // the send's bound
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG).empty());
    REQUIRE(LinesAt(LOG_LEVEL_WARNING).empty());
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == kSendLines);
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 50);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_DONE);
    REQUIRE(g_published[0].measurement.match_count == 144);
}

// ==== 3. The flag cannot get stuck =================================================================================

namespace {
/** After the flag was cleared: the next arm (of @p mode) calls rmt_receive() once and succeeds. */
void RequireNextArmSucceeds(Mode mode, uint32_t submit_seq)
{
    ResetFakes();
    g_published.clear();
    const uint32_t armed_before = HarnessGetArmedSeq();
    Arm(mode, submit_seq);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(HarnessGetArmedSeq() == armed_before + 1);
    REQUIRE(HarnessGetCaptureSeq() == armed_before + 1);
    REQUIRE(HarnessGetArmedSubmitSeq() == submit_seq);
    REQUIRE(HarnessIsReceivePending());
    REQUIRE(g_published.empty());
    REQUIRE(LinesAt(LOG_LEVEL_WARNING).empty());
}
}  // namespace

TEST_CASE("flag: cleared by the done ISR; the next arm of either mode succeeds", "[T-5][FR-13]")
{
    for (Mode first : {Mode::Read, Mode::Send}) {
        for (Mode next : {Mode::Read, Mode::Send}) {
            DYNAMIC_SECTION(Name(first) << " done, then a " << Name(next) << " arm")
            {
                StartAndArm(first, 60);
                ListenForResults();
                FireDoneIsr();
                REQUIRE_FALSE(HarnessIsReceivePending());
                REQUIRE(LogWrite_fake.call_count == 0);   // the ISR never logs
                RequireNextArmSucceeds(next, 61);
            }
        }
    }
}

TEST_CASE("flag: cleared by the restart after a timeout, even if rmt_disable or rmt_enable fails", "[T-5][FR-12][FR-13][NFR-11]")
{
    for (Mode mode : {Mode::Read, Mode::Send}) {
        DYNAMIC_SECTION(Name(mode) << " times out, restart succeeds")
        {
            StartAndArm(mode, 70);
            ListenForResults();
            const harness_capture_t captures[] = {Timeout()};
            HarnessRunDecodeTask(captures, 1, pdTRUE);
            REQUIRE(rmt_disable_fake.call_count == 1);
            REQUIRE(rmt_enable_fake.call_count == 1);
            REQUIRE_FALSE(HarnessIsReceivePending());
            REQUIRE(g_published.back().measurement.state == WS2812_MEASUREMENT_TIMEOUT);
            RequireNextArmSucceeds(Mode::Send, 71);
        }
        DYNAMIC_SECTION(Name(mode) << " times out, rmt_disable fails")
        {
            StartAndArm(mode, 72);
            ListenForResults();
            rmt_disable_fake.return_val = ESP_FAIL;
            const harness_capture_t captures[] = {Timeout()};
            HarnessRunDecodeTask(captures, 1, pdTRUE);
            REQUIRE(rmt_enable_fake.call_count == 0);
            REQUIRE(LogWrite_fake.call_count >= 2);
            REQUIRE(Log().find("pulse monitor: rx restart failed (err=-1)") != std::string::npos);
            REQUIRE_FALSE(HarnessIsReceivePending());
            RequireNextArmSucceeds(Mode::Read, 73);
        }
        DYNAMIC_SECTION(Name(mode) << " times out, rmt_enable fails")
        {
            StartAndArm(mode, 74);
            ListenForResults();
            rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
            const harness_capture_t captures[] = {Timeout()};
            HarnessRunDecodeTask(captures, 1, pdTRUE);
            REQUIRE(rmt_disable_fake.call_count == 1);
            REQUIRE(Log().find("pulse monitor: rx restart failed (err=259)") != std::string::npos);
            REQUIRE_FALSE(HarnessIsReceivePending());
            // The channel was left disabled: the next arm gets INVALID_STATE, re-enables once and retries (FR-24).
            ResetFakes();
            g_published.clear();
            rmt_receive_fake.custom_fake = [](rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *) {
                return rmt_receive_fake.call_count == 1 ? ESP_ERR_INVALID_STATE : ESP_OK;
            };
            Arm(Mode::Read, 75);
            REQUIRE(rmt_receive_fake.call_count == 2);
            REQUIRE(rmt_enable_fake.call_count == 1);
            REQUIRE(HarnessGetArmedSubmitSeq() == 75);
            REQUIRE(HarnessIsReceivePending());
            REQUIRE(g_published.empty());
        }
    }
}

TEST_CASE("flag: cleared by a failed rmt_receive (plain failure and the re-enable-retry failure)", "[T-5][FR-11][FR-13]")
{
    for (Mode mode : {Mode::Read, Mode::Send}) {
        DYNAMIC_SECTION(Name(mode) << ": plain failure (ESP_ERR_INVALID_ARG)")
        {
            StartMonitor();
            ListenForResults();
            rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
            Arm(mode, 80);
            REQUIRE(rmt_receive_fake.call_count == 1);
            REQUIRE_FALSE(HarnessIsReceivePending());
            REQUIRE(Log().find("pulse monitor: arm failed (err=258)") != std::string::npos);
            RequireNextArmSucceeds(mode, 81);
        }
        DYNAMIC_SECTION(Name(mode) << ": re-enable succeeds, the retry fails")
        {
            StartMonitor();
            ListenForResults();
            rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
            Arm(mode, 82);
            REQUIRE(rmt_receive_fake.call_count == 2);
            REQUIRE(rmt_enable_fake.call_count == 1);
            REQUIRE_FALSE(HarnessIsReceivePending());
            RequireNextArmSucceeds(mode, 83);
        }
        DYNAMIC_SECTION(Name(mode) << ": INVALID_STATE and rmt_enable rejected")
        {
            StartMonitor();
            ListenForResults();
            rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
            rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
            Arm(mode, 84);
            REQUIRE(rmt_receive_fake.call_count == 1);
            REQUIRE_FALSE(HarnessIsReceivePending());
            RequireNextArmSucceeds(mode, 85);
        }
    }
}

namespace {
void EndAndArmNewerSend()
{
    HarnessEndReceive();   // a late done event ended the timed-out receive ...
    ArmPulseCapture(&kVectorA, 91, kPattern, sizeof(kPattern));   // ... and a newer arm succeeded
    uxSemaphoreGetCount_fake.return_val = 1;
}
}  // namespace

TEST_CASE("flag: a restart skipped because a newer arm exists leaves the newer arm's receive pending", "[T-5][FR-12][FR-13]")
{
    StartAndArm(Mode::Read, 90);
    ListenForResults();
    const harness_capture_t captures[] = {Timeout(EndAndArmNewerSend)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(HarnessGetArmedSeq() == 2);
    REQUIRE(rmt_disable_fake.call_count == 0);           // restart skipped
    REQUIRE(HarnessIsReceivePending());                  // the newer send's receive still owns the channel
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 90);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);

    // So a further arm is still rejected without touching the driver.
    ResetFakes();
    g_published.clear();
    Arm(Mode::Read, 92);
    RequireBusyArmRejected(92, 2, 2);
}

TEST_CASE("flag: a stale event does not end the newer receive still in flight", "[T-5][FR-13]")
{
    StartAndArm(Mode::Send, 93);
    FireDoneIsr();                                       // arm 1's receive ends (its event is still unconsumed)
    Arm(Mode::Read, 94);                                 // arm 2 in flight
    REQUIRE(HarnessIsReceivePending());
    ResetFakes();
    // The decode task consumes arm 1's (stale) event and keeps waiting for arm 2, which then times out.
    const harness_capture_t captures[] = {Arrive(144, 1, +[] {
        REQUIRE(HarnessIsReceivePending());              // still pending while the stale event is delivered
    }), Timeout()};
    HarnessRunDecodeTask(captures, 2, pdTRUE);
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG).front() == "pulse monitor: stale capture discarded (seq=1)");
    REQUIRE(HarnessCaptureWaitTicks(1) == 100);
    REQUIRE_FALSE(HarnessIsReceivePending());            // cleared by arm 2's timeout restart
}

// ==== 4. Regression: non-overlapping Send, Read, Send ==============================================================

TEST_CASE("regression: Send, Read, Send without overlap: same receive calls, logs and publications as before the fix", "[T-5][FR-13][SPEC-004]")
{
    StartMonitor();
    ListenForResults();
    std::vector<std::string> info;

    // Send 1
    Arm(Mode::Send, 1);
    FillIdealSend();
    const harness_capture_t send1[] = {Arrive(144)};
    HarnessRunDecodeTask(send1, 1, pdTRUE);
    REQUIRE(HarnessCaptureWaitTicks(0) == 2);
    REQUIRE_FALSE(HarnessIsReceivePending());
    // Read 2
    Arm(Mode::Read, 2);
    FillR1();
    const harness_capture_t read2[] = {Arrive(144)};
    HarnessRunDecodeTask(read2, 1, pdTRUE);
    REQUIRE(HarnessCaptureWaitTicks(0) == 100);
    // Send 3
    Arm(Mode::Send, 3);
    FillIdealSend();
    const harness_capture_t send3[] = {Arrive(144)};
    HarnessRunDecodeTask(send3, 1, pdTRUE);

    REQUIRE(rmt_receive_fake.call_count == 3);
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(HarnessGetArmedSeq() == 3);
    std::vector<std::string> expected = kSendLines;
    expected.push_back(kR1Line);
    expected.insert(expected.end(), kSendLines.begin(), kSendLines.end());
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == expected);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG).empty());
    REQUIRE(g_published.size() == 3);
    REQUIRE(g_published[0].measurement.submit_seq == 1);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_DONE);
    REQUIRE(g_published[0].measurement.match_count == 144);
    REQUIRE(g_published[1].measurement.submit_seq == 2);
    REQUIRE(g_published[1].measurement.state == WS2812_MEASUREMENT_READ_DONE);
    REQUIRE(g_published[1].measurement.bit0_high_avg_ns == 400);
    REQUIRE(g_published[1].measurement.bit1_high_avg_ns == 800);
    REQUIRE(g_published[2].measurement.submit_seq == 3);
    REQUIRE(g_published[2].measurement.state == WS2812_MEASUREMENT_DONE);
    for (const Published &published : g_published) {
        REQUIRE(published.rx_lock_balance == 0);
    }
}
