/**
 * @file test_read_pulse_monitor.cpp
 * @brief SPEC-006 T-5 (FR-11 to FR-14, FR-16, FR-17, NFR-3, NFR-4): ArmPulseRead() and the read mode of the decode
 *        task in main/rmt_pulse_monitor/rmt_pulse_monitor.c.
 *
 * The file is compiled through mocks/rmt_pulse_monitor_read_harness.c (an extension of the led-controller harness), so
 * its statics, the ISR callback and the endless decode task are reachable. RMT RX and FreeRTOS are FFF fakes; the
 * decode loop is run by HarnessRunDecodeTask() with scripted capture outcomes.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
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
const char *const kR1Line = "pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; bits=143";

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

void ResetAll()
{
    ResetFakes();
    HarnessResetReadPulseMonitor();
    SetPulseResultCallback(nullptr);
}

void StartMonitor()
{
    ResetAll();
    REQUIRE(StartPulseMonitor());
    ResetFakes();
}

void StartAndArmRead(uint32_t submit_seq)
{
    StartMonitor();
    ArmPulseRead(submit_seq);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(HarnessIsArmedModeRead());
    ResetFakes();
}

void StartAndArmSend(uint32_t submit_seq)
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, submit_seq, kPattern, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    ResetFakes();
}

rmt_symbol_word_t Sym(uint32_t d0, uint32_t d1, uint32_t level0 = 1, uint32_t level1 = 0)
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = d0;
    symbol.level0 = level0;
    symbol.duration1 = d1;
    symbol.level1 = level1;
    return symbol;
}

/** Vector R1 in the static capture buffer: 143 alternating (16,34)/(32,18) and a final (16,0); returns 144. */
size_t FillR1()
{
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < 143; ++index) {
        buffer[index] = index % 2 == 0 ? Sym(16, 34) : Sym(32, 18);
    }
    buffer[143] = Sym(16, 0);
    return 144;
}

/** @p count copies of (d0, d1) and a final (d0, 0) at the start of the capture buffer; returns count + 1. */
size_t FillUniform(size_t count, uint32_t d0, uint32_t d1)
{
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    std::memset(buffer, 0, HarnessGetSymbolBufferBytes());
    for (size_t index = 0; index < count; ++index) {
        buffer[index] = Sym(d0, d1);
    }
    buffer[count] = Sym(d0, 0);
    return count + 1;
}

void ArmReadAndFillR1()
{
    ArmPulseRead(61);
    FillR1();
}

std::string Log() { return TestLogText(); }
bool LogContains(const std::string &text) { return Log().find(text) != std::string::npos; }

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
    int rx_lock_balance;   // decode path: takes minus gives at the call (0 = lock not held)
    bool rx_lock_held;     // arm path: tracked by the lock fakes below
    int info_before;
    int warnings_before;
};
std::vector<Published> g_published;
bool g_rx_lock_held = false;
bool g_rx_lock_busy = false;

void RecordPublished(const ws2812_measurement_t *measurement)
{
    g_published.push_back({*measurement, RxLockBalance(), g_rx_lock_held, TestLogCount(LOG_LEVEL_INFO),
                           TestLogCount(LOG_LEVEL_WARNING)});
}

void ListenForResults()
{
    g_published.clear();
    SetPulseResultCallback(RecordPublished);
}

BaseType_t TrackingTake(SemaphoreHandle_t semaphore, TickType_t)
{
    if (semaphore == HarnessGetRxLock()) {
        if (g_rx_lock_busy) {
            return pdFALSE;
        }
        g_rx_lock_held = true;
    }
    return pdTRUE;
}

BaseType_t TrackingGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == HarnessGetRxLock()) {
        g_rx_lock_held = false;
    }
    return pdTRUE;
}

void TrackRxLock(bool busy)
{
    g_rx_lock_held = false;
    g_rx_lock_busy = busy;
    xSemaphoreTake_fake.custom_fake = TrackingTake;
    xSemaphoreGive_fake.custom_fake = TrackingGive;
}

void UntrackRxLock()
{
    xSemaphoreTake_fake.custom_fake = nullptr;
    xSemaphoreGive_fake.custom_fake = nullptr;
}

void RequireOneOutcome(uint32_t submit_seq, ws2812_measurement_state_t state)
{
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == submit_seq);
    REQUIRE(g_published[0].measurement.state == state);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit1_high_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit0_period_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit1_period_avg_ns == 0);
    REQUIRE_FALSE(g_published[0].measurement.match_available);
}

std::string ReadSource(const char *path)
{
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

// ==== FR-11: ArmPulseRead() ===========================================================================================

TEST_CASE("read arm: rmt_receive on the static 144-symbol buffer with the SPEC-004 limits, no transmit", "[T-5][FR-11][FR-9]")
{
    StartMonitor();
    ListenForResults();
    ArmPulseRead(31);

    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(rmt_receive_fake.arg0_val == kFakeRxChannel);
    REQUIRE(rmt_receive_fake.arg1_val == HarnessGetSymbolBuffer());
    REQUIRE(rmt_receive_fake.arg2_val == 576);
    REQUIRE(g_last_receive_config.signal_range_min_ns == 50);
    REQUIRE(g_last_receive_config.signal_range_max_ns == 25000);
    REQUIRE(rmt_transmit_fake.call_count == 0);         // FR-9: generates nothing
    REQUIRE(rmt_new_tx_channel_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(LogWrite_fake.call_count == 0);
    REQUIRE(g_published.empty());                       // a successful arm publishes nothing
}

TEST_CASE("read arm: zero-timeout lock, next arm number, mode read and submit_seq stored, semaphore given, lock released last", "[T-5][FR-11][NFR-3]")
{
    StartMonitor();
    const uint32_t seq_before = HarnessGetArmedSeq();
    ArmPulseRead(32);

    REQUIRE(xSemaphoreTake_fake.call_count == 1);
    REQUIRE(xSemaphoreTake_fake.arg0_val == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_val == 0);          // never blocks
    REQUIRE(HarnessGetArmedSeq() == seq_before + 1);
    REQUIRE(HarnessGetCaptureSeq() == seq_before + 1);
    REQUIRE(HarnessIsArmedModeRead());
    REQUIRE(HarnessGetArmedSubmitSeq() == 32);
    REQUIRE(xSemaphoreGive_fake.call_count == 2);
    REQUIRE(xSemaphoreGive_fake.arg0_history[0] == HarnessGetArmedSemaphore());
    REQUIRE(xSemaphoreGive_fake.arg0_history[1] == HarnessGetRxLock());
    REQUIRE(xQueueReceive_fake.call_count == 0);         // no wait, no busy loop
}

TEST_CASE("read arm stores no timing or pixel data: the previous send snapshot is left as it was", "[T-5][FR-11]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, 10, kPattern, sizeof(kPattern));
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    HarnessEndReceive();   // SPEC-006 FR-13: the earlier receive has ended (done ISR), so the channel is free
    ArmPulseRead(11);
    REQUIRE(HarnessIsArmedModeRead());
    const ws2812_timing_t armed = HarnessGetArmedTiming();
    REQUIRE(std::memcmp(&armed, &kVectorA, sizeof(armed)) == 0);
    REQUIRE(HarnessGetArmedPixelLength() == sizeof(kPattern));
    REQUIRE(HarnessGetArmedSubmitSeq() == 11);
}

TEST_CASE("arm numbers are shared by both modes; a send arm after a read arm stores mode send again", "[T-5][FR-11]")
{
    StartMonitor();
    ArmPulseRead(1);
    REQUIRE(HarnessGetArmedSeq() == 1);
    HarnessEndReceive();   // SPEC-006 FR-13: the earlier receive has ended (done ISR), so the channel is free
    ArmPulseCapture(&kVectorA, 2, kPattern, sizeof(kPattern));
    REQUIRE(HarnessGetArmedSeq() == 2);
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    HarnessEndReceive();   // SPEC-006 FR-13: the earlier receive has ended (done ISR), so the channel is free
    ArmPulseRead(3);
    REQUIRE(HarnessGetArmedSeq() == 3);
    REQUIRE(HarnessIsArmedModeRead());
}

TEST_CASE("read arm: a disabled channel is re-enabled once and the receive retried once", "[T-5][FR-11]")
{
    StartMonitor();
    ListenForResults();
    rmt_receive_fake.custom_fake = [](rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *) {
        return rmt_receive_fake.call_count == 1 ? ESP_ERR_INVALID_STATE : ESP_OK;   // disabled, then OK after enable
    };
    ArmPulseRead(33);
    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(HarnessIsArmedModeRead());
    REQUIRE(HarnessGetArmedSubmitSeq() == 33);
    REQUIRE(g_published.empty());
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
}

TEST_CASE("read arm failures publish not_measured after releasing the lock, with the existing Warnings", "[T-5][FR-11][FR-17]")
{
    SECTION("monitor not started: no channel, no Warning (as ArmPulseCapture)")
    {
        ResetAll();
        ListenForResults();
        TrackRxLock(false);
        ArmPulseRead(80);
        RequireOneOutcome(80, WS2812_MEASUREMENT_NOT_MEASURED);
        REQUIRE(rmt_receive_fake.call_count == 0);
        REQUIRE(LogWrite_fake.call_count == 0);
    }
    SECTION("RX lock busy")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(true);
        ArmPulseRead(81);
        RequireOneOutcome(81, WS2812_MEASUREMENT_NOT_MEASURED);
        REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm skipped (reason=rx_restart_busy)"});
        REQUIRE(g_published[0].warnings_before == 1);
        REQUIRE(rmt_receive_fake.call_count == 0);
    }
    SECTION("rmt_receive fails with another error")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
        ArmPulseRead(82);
        RequireOneOutcome(82, WS2812_MEASUREMENT_NOT_MEASURED);
        REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=258)"});
        REQUIRE(g_published[0].warnings_before == 1);
        REQUIRE(rmt_enable_fake.call_count == 0);
    }
    SECTION("re-enable succeeds but the retry fails")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
        ArmPulseRead(83);
        RequireOneOutcome(83, WS2812_MEASUREMENT_NOT_MEASURED);
        REQUIRE(rmt_receive_fake.call_count == 2);
        REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    }
    SECTION("a capture is pending: INVALID_STATE and rmt_enable rejected")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
        rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
        ArmPulseRead(84);
        RequireOneOutcome(84, WS2812_MEASUREMENT_NOT_MEASURED);
        REQUIRE(rmt_receive_fake.call_count == 1);
        REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    }
    REQUIRE_FALSE(g_published[0].rx_lock_held);
    REQUIRE_FALSE(g_rx_lock_held);
    REQUIRE(rmt_transmit_fake.call_count == 0);
    UntrackRxLock();
}

TEST_CASE("a failed read arm keeps the armed snapshot (number, mode, submit_seq) and restores the ISR tag", "[T-5][FR-11][FR-13]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, 20, kPattern, sizeof(kPattern));   // seq 1, send, still pending
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    ArmPulseRead(21);
    REQUIRE(HarnessGetArmedSeq() == 1);
    REQUIRE(HarnessGetCaptureSeq() == 1);
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    REQUIRE(HarnessGetArmedSubmitSeq() == 20);
}

// ==== FR-12 / NFR-4: per-mode wait and timeout ========================================================================

TEST_CASE("decode task waits 100 ticks (1,000 ms) after a read arm and 2 ticks (20 ms) after a send arm", "[T-5][FR-12][NFR-4]")
{
    SECTION("read arm")
    {
        StartAndArmRead(40);
        const harness_capture_t captures[] = {Arrive(FillR1())};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(HarnessCaptureWaitTicks(0) == pdMS_TO_TICKS(RMT_PULSE_MONITOR_READ_TIMEOUT_MS));
        REQUIRE(HarnessCaptureWaitTicks(0) == 100);
    }
    SECTION("send arm")
    {
        StartAndArmSend(41);
        const harness_capture_t captures[] = {Timeout()};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(HarnessCaptureWaitTicks(0) == 2);
    }
    REQUIRE(xQueueReceive_fake.arg0_history[0] == HarnessGetCaptureQueue());
}

TEST_CASE("read timeout: one Warning, RX restart, then timeout published for the read after the lock is released", "[T-5][FR-12][FR-17]")
{
    StartAndArmRead(42);
    ListenForResults();
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(HarnessCaptureWaitTicks(0) == 100);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(RxLockBalance() == 0);
    RequireOneOutcome(42, WS2812_MEASUREMENT_TIMEOUT);
    REQUIRE(g_published[0].rx_lock_balance == 0);
    REQUIRE(g_published[0].warnings_before == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
}

TEST_CASE("read timeout: the restart is skipped if a newer arm exists; timeout goes to the waited read", "[T-5][FR-12]")
{
    StartAndArmRead(43);
    ListenForResults();
    const harness_capture_t captures[] = {Timeout(+[] {
        HarnessEndReceive();   // SPEC-006 FR-13: a late done event ended the read's receive
        ArmPulseCapture(&kVectorA, 44, kPattern, sizeof(kPattern));   // a newer send arm during the read wait
        uxSemaphoreGetCount_fake.return_val = 1;
    })};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 43);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
}

TEST_CASE("read timeout, then a send capture is measured normally (no reboot)", "[T-5][FR-12][NFR-11]")
{
    StartAndArmRead(45);
    ListenForResults();
    const harness_capture_t timeout[] = {Timeout()};
    HarnessRunDecodeTask(timeout, 1, pdTRUE);
    REQUIRE(g_published.back().measurement.state == WS2812_MEASUREMENT_TIMEOUT);

    ResetFakes();
    ArmPulseCapture(&kVectorA, 46, kPattern, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    const harness_capture_t count_error[] = {Arrive(143)};
    HarnessRunDecodeTask(count_error, 1, pdTRUE);
    REQUIRE(HarnessCaptureWaitTicks(0) == 2);   // the send's own 20 ms bound
    REQUIRE(g_published.back().measurement.submit_seq == 46);
    // A send keeps the SPEC-004 FR-32 count check (143 != 144).
    REQUIRE(g_published.back().measurement.state == WS2812_MEASUREMENT_COUNT_ERROR);
    REQUIRE(LogContains("pulse monitor: symbol count mismatch (reason=symbol_count count=143)"));
}

// ==== FR-14 / FR-16 / FR-17: analysis, terminal line, publication =====================================================

TEST_CASE("R1 read capture: READ_DONE with the FR-15 averages and exactly the FR-16 Info line", "[T-5][FR-14][FR-16][FR-17][FR-18]")
{
    StartAndArmRead(50);
    ListenForResults();
    const harness_capture_t captures[] = {Arrive(FillR1())};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);

    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{kR1Line});
    REQUIRE(Log() == std::string("[L1 pulse_mon] ") + kR1Line + "\n");   // nothing else at any level
    REQUIRE(g_published.size() == 1);
    const ws2812_measurement_t &m = g_published[0].measurement;
    REQUIRE(m.submit_seq == 50);
    REQUIRE(m.state == WS2812_MEASUREMENT_READ_DONE);
    REQUIRE(m.bit0_high_avg_ns == 400);
    REQUIRE(m.bit0_period_avg_ns == 1250);
    REQUIRE(m.bit1_high_avg_ns == 800);
    REQUIRE(m.bit1_period_avg_ns == 1250);
    REQUIRE(m.match_count == 0);
    REQUIRE_FALSE(m.match_available);
    REQUIRE(g_published[0].rx_lock_balance == 0);   // published after the RX lock was released
    REQUIRE(g_published[0].info_before == 1);       // after the terminal line
    REQUIRE(HarnessIsDecodeModeRead());
    REQUIRE(HarnessGetDecodeSubmitSeq() == 50);
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(rmt_transmit_fake.call_count == 0);
}

TEST_CASE("read capture: no SPEC-004 count check, decoding or section 7.6 lines", "[T-5][FR-14]")
{
    StartAndArmRead(51);
    ListenForResults();
    SECTION("a short 31-symbol frame (no 144 required)")
    {
        const harness_capture_t captures[] = {Arrive(FillUniform(30, 16, 34))};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(LinesAt(LOG_LEVEL_INFO) ==
                std::vector<std::string>{"pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=n/a period_ns=n/a; bits=30"});
    }
    SECTION("an R1 frame with a level0 = 0 symbol (no bad_symbol Warning)")
    {
        const size_t count = FillR1();
        HarnessGetSymbolBuffer()[0].level0 = 0;
        HarnessGetSymbolBuffer()[0].level1 = 1;
        const harness_capture_t captures[] = {Arrive(count)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(LinesAt(LOG_LEVEL_INFO) ==
                std::vector<std::string>{"pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; bits=142"});
    }
    REQUIRE_FALSE(LogContains("symbol count mismatch"));
    REQUIRE_FALSE(LogContains("symbol decode error"));
    REQUIRE_FALSE(LogContains("pulse: "));
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
}

TEST_CASE("read capture with 3 usable symbols: Warning 'too few bits (count=3)' and count_error", "[T-5][FR-14][FR-17]")
{
    StartAndArmRead(52);
    ListenForResults();
    const harness_capture_t captures[] = {Arrive(FillUniform(3, 16, 34))};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse read: too few bits (count=3)"});
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    RequireOneOutcome(52, WS2812_MEASUREMENT_COUNT_ERROR);
    REQUIRE(g_published[0].rx_lock_balance == 0);
    REQUIRE(g_published[0].warnings_before == 1);
}

TEST_CASE("read capture: the count reported is the usable count, not the symbol count", "[T-5][FR-14]")
{
    StartAndArmRead(53);
    ListenForResults();
    // 10 symbols captured from an idle-high (inverted) source: level0 = 0 everywhere -> 0 usable.
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < 10; ++index) {
        buffer[index] = Sym(34, 16, 0, 1);
    }
    const harness_capture_t captures[] = {Arrive(10)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse read: too few bits (count=0)"});
    RequireOneOutcome(53, WS2812_MEASUREMENT_COUNT_ERROR);
}

TEST_CASE("read capture: 8 usable bits is enough (minimum boundary)", "[T-5][FR-14]")
{
    StartAndArmRead(54);
    ListenForResults();
    const harness_capture_t captures[] = {Arrive(FillUniform(8, 32, 18))};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(LinesAt(LOG_LEVEL_INFO) ==
            std::vector<std::string>{"pulse read: bit0 high_ns=n/a period_ns=n/a; bit1 high_ns=800 period_ns=1250; bits=8"});
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit0_period_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit1_high_avg_ns == 800);
}

TEST_CASE("read capture: the longest FR-16 line stays within 127 characters", "[T-5][FR-16]")
{
    StartAndArmRead(55);
    // Two classes at the extremes: 71 x (32767 ticks high) and 72 x (4 ticks high), periods near the 15-bit maximum.
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < 143; ++index) {
        buffer[index] = index % 2 == 0 ? Sym(4, 32767) : Sym(32767, 32767);
    }
    buffer[143] = Sym(4, 0);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    const std::vector<std::string> lines = LinesAt(LOG_LEVEL_INFO);
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "pulse read: bit0 high_ns=100 period_ns=819275; bit1 high_ns=819175 period_ns=1638350; bits=143");
    REQUIRE(lines[0].size() <= 127);
}

TEST_CASE("read capture: no callback registered -> logs the same line and does not crash", "[T-5][FR-17]")
{
    StartAndArmRead(56);
    const harness_capture_t captures[] = {Arrive(FillR1())};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{kR1Line});
}

TEST_CASE("read capture: tag compare and copy under the RX lock, analysis and publication after its release", "[T-5][FR-17]")
{
    StartAndArmRead(57);
    ListenForResults();
    const harness_capture_t captures[] = {Arrive(FillR1())};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(xSemaphoreTake_fake.arg0_history[2] == HarnessGetRxLock());   // [0] armed sem, [1] GetArmedSeq, [2] copy
    REQUIRE(xSemaphoreGive_fake.call_count == 2);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].rx_lock_balance == 0);
}

TEST_CASE("read capture: the decode uses its private copy; an arm right after the copy changes nothing", "[T-5][FR-12][FR-17]")
{
    StartAndArmRead(58);
    ListenForResults();
    FillR1();
    // Fire on the release after TakeCurrentCapture()'s copy: a new send arm and a refilled buffer.
    HarnessSetAfterRxLockReleaseHook(+[] {
        ArmPulseCapture(&kVectorA, 59, kPattern, sizeof(kPattern));
        FillUniform(3, 16, 34);
    }, 1);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    HarnessSetAfterRxLockReleaseHook(nullptr, 0);
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{kR1Line});
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 58);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
}

TEST_CASE("stale events apply to read arms: a send's old event is discarded, the wait moves to the read with 100 ticks", "[T-5][FR-12]")
{
    StartAndArmSend(60);   // arm 1 (send)
    ListenForResults();
    // The ISR tags the send's event with arm 1; before the decode task takes the lock, a read arms (arm 2) and the
    // source frame refills the buffer.
    const harness_capture_t captures[] = {Arrive(144, 0, ArmReadAndFillR1), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);
    REQUIRE(HarnessCaptureWaitTicks(0) == 2);     // the send's bound
    REQUIRE(HarnessCaptureWaitTicks(1) == 100);   // the read's bound after the stale discard
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG) == std::vector<std::string>{"pulse monitor: stale capture discarded (seq=1)"});
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{kR1Line});
    REQUIRE(g_published.size() == 1);             // nothing for the stale send arm
    REQUIRE(g_published[0].measurement.submit_seq == 61);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
}

// ==== FR-13: overlap ================================================================================================

TEST_CASE("FR-13: a Send arm while a read is pending fails, publishes not_measured for the Send; the Read completes", "[T-5][FR-13]")
{
    StartAndArmRead(70);
    ListenForResults();
    // The read's receive is still running (s_is_receive_pending): the Send fails before rmt_receive() (2026-10-04
    // overlap fix). The driver stubs below would only matter if rmt_receive() were reached.
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    ArmPulseCapture(&kVectorA, 71, kPattern, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(HarnessGetCaptureSeq() == 1);
    RequireOneOutcome(71, WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    REQUIRE(HarnessIsArmedModeRead());                     // the pending read is untouched
    REQUIRE(HarnessGetArmedSubmitSeq() == 70);
    REQUIRE(HarnessGetArmedSeq() == 1);

    ResetFakes();
    g_published.clear();
    const harness_capture_t captures[] = {Arrive(FillR1())};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 70);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_READ_DONE);
}

TEST_CASE("FR-13: a Read while a send capture is pending fails and publishes not_measured for the Read", "[T-5][FR-13]")
{
    StartAndArmSend(72);
    ListenForResults();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    ArmPulseRead(73);
    RequireOneOutcome(73, WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    REQUIRE_FALSE(HarnessIsArmedModeRead());
    REQUIRE(HarnessGetArmedSubmitSeq() == 72);
    // SPEC-006 FR-13 tag-race fix (2026-10-04): the send receive is still in flight, so the Read fails before
    // rmt_receive() (was 1 call); nothing queued or retried.
    REQUIRE(rmt_receive_fake.call_count == 0);
}

// ==== Static review =================================================================================================

TEST_CASE("static: ArmPulseRead touches no TX, GPIO or led_controller API and never waits", "[T-5][FR-9][NFR-3]")
{
    const std::string source = ReadSource(RMT_PULSE_MONITOR_SRC);
    const size_t begin = source.find("void ArmPulseRead(uint32_t submit_seq)");
    REQUIRE(begin != std::string::npos);
    const size_t end = source.find("\n}\n", begin);
    REQUIRE(end != std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    for (const char *token : {"rmt_transmit", "gpio_", "ApplyWs2812Timing", "ArmPulseCapture", "portMAX_DELAY",
                              "xQueueReceive", "vTaskDelay", "malloc"}) {
        INFO(token);
        REQUIRE(body.find(token) == std::string::npos);
    }
    REQUIRE(body.find("xSemaphoreTake(s_rx_lock, 0)") != std::string::npos);
}
