/**
 * @file test_rmt_pulse_monitor.cpp
 * @brief Host tests for rmt_pulse_monitor.c through a harness that compiles the file (SPEC-004
 *        FR-19, FR-20, FR-22, FR-24, FR-25, FR-29..FR-32; host parts of T-15/T-17 behavior).
 *
 * RMT RX calls and FreeRTOS are FFF fakes. The decode task's endless loop is run by
 * HarnessRunDecodeTask() with a script of capture outcomes (event arrives / 20 ms wait times out).
 */
#include <catch2/catch_test_macros.hpp>
#include <array>
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
#include "rmt_pulse_monitor_harness.h"
}

namespace {

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};
// 2026-10-03: SPEC-003 V4 makes B (equal highs, equal periods) and D (inverted highs, equal periods) unreachable
// through /tuner. Every arm/decode path below uses the reachable counterparts instead: AC (equal highs, shorter
// bit-1 period, match n/a) and AD (inverted highs, valid duty order). B and D stay as pure inputs in test_led_pure.cpp.
const ws2812_timing_t kVectorAC = {500, 1250, 500, 1000, 280};
const ws2812_timing_t kVectorAD = {600, 2000, 500, 1000, 280};
/** submit_seq used where a test does not care about the number (FR-24 signature since 2026-10-03). */
constexpr uint32_t kAnySeq = 100;
const uint8_t kPattern[18] = {
    0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x20,
};

/** A capture-done event tagged by the ISR for the current arm (or @p stale_by arms older). */
harness_capture_t Arrive(size_t symbol_count, uint32_t stale_by = 0, void (*before_delivery)() = nullptr)
{
    return harness_capture_t{true, symbol_count, stale_by, before_delivery};
}

/** The 20 ms capture wait times out. */
harness_capture_t Timeout() { return harness_capture_t{false, 0, 0, nullptr}; }

void ResetAll()
{
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    HarnessResetPulseMonitor();
    FFF_RESET_HISTORY();
}

/** Start the monitor with every fake succeeding, then forget the start-up calls. */
void StartMonitor()
{
    ResetAll();
    REQUIRE(StartPulseMonitor());
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
}

/** Start and arm one capture for @p timing and the fixed pattern, then forget those calls. */
void StartAndArm(const ws2812_timing_t &timing = kVectorA, uint32_t submit_seq = kAnySeq)
{
    StartMonitor();
    ArmPulseCapture(&timing, submit_seq, kPattern, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 1);
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
}

int HistoryIndex(void *function, int occurrence = 0)
{
    for (unsigned index = 0; index < fff.call_history_idx; ++index) {
        if (reinterpret_cast<void *>(fff.call_history[index]) == function && occurrence-- == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

template <typename F>
void *Fn(F *function)
{
    return reinterpret_cast<void *>(function);
}

int CountCalls(void *function)
{
    int count = 0;
    for (unsigned index = 0; index < fff.call_history_idx; ++index) {
        count += (reinterpret_cast<void *>(fff.call_history[index]) == function);
    }
    return count;
}

/** RX-lock takes minus gives since the last FreeRTOS fake reset (0 = every take was released). */
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

std::string Log() { return TestLogText(); }
bool LogContains(const std::string &text) { return Log().find(text) != std::string::npos; }
bool SameTiming(const ws2812_timing_t &a, const ws2812_timing_t &b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

/** All captured log lines at @p level, without the "[L<n> <module>] " prefix. */
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

/** Fill the static capture buffer with an ideal capture of the fixed pattern at @p timing. */
void FillIdealCapture(const ws2812_timing_t &timing)
{
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < RMT_PULSE_MONITOR_DATA_SYMBOLS; ++index) {
        const bool bit = ((kPattern[index / 8] >> (7 - index % 8)) & 1) != 0;
        const uint32_t high_ns = bit ? timing.bit1_high_ns : timing.bit0_high_ns;
        const uint32_t period_ns = bit ? timing.bit1_period_ns : timing.bit0_period_ns;
        rmt_symbol_word_t symbol{};
        symbol.duration0 = high_ns / 25;
        symbol.level0 = 1;
        symbol.duration1 = (period_ns - high_ns) / 25;
        symbol.level1 = 0;
        buffer[index] = symbol;
    }
    buffer[RMT_PULSE_MONITOR_DATA_SYMBOLS - 1].duration1 = 0;   // RMT end marker
}

// Contents of the capture-done event handed to xQueueOverwriteFromISR() by the ISR callback.
size_t g_isr_event_count;
uint32_t g_isr_event_seq;
BaseType_t OverwriteFromIsrWakes(QueueHandle_t, const void *item, BaseType_t *woken)
{
    HarnessReadCaptureEvent(item, &g_isr_event_count, &g_isr_event_seq);
    *woken = pdTRUE;
    return pdTRUE;
}

// ArmPulseCapture's state, as seen at the moment(s) rmt_receive() is called.
ws2812_timing_t g_timing_at_receive;
uint32_t g_capture_seq_at_receive[4];
uint32_t g_armed_seq_at_receive[4];
int g_receive_calls;
std::vector<esp_err_t> g_receive_results;   // per call; ESP_OK once exhausted
esp_err_t ReceiveRecordsState(rmt_channel_handle_t, void *, size_t, const rmt_receive_config_t *config)
{
    g_last_receive_config = *config;
    g_timing_at_receive = HarnessGetArmedTiming();
    if (g_receive_calls < 4) {
        g_capture_seq_at_receive[g_receive_calls] = HarnessGetCaptureSeq();
        g_armed_seq_at_receive[g_receive_calls] = HarnessGetArmedSeq();
    }
    const size_t call = static_cast<size_t>(g_receive_calls++);
    return call < g_receive_results.size() ? g_receive_results[call] : ESP_OK;
}

void RecordReceives(std::vector<esp_err_t> results = {})
{
    g_receive_calls = 0;
    g_receive_results = std::move(results);
    rmt_receive_fake.custom_fake = ReceiveRecordsState;
}

/** Arm @p timing with the fixed pattern and fill the capture buffer with its ideal capture (race hooks). */
void ArmVectorADAndRefill()
{
    ArmPulseCapture(&kVectorAD, kAnySeq, kPattern, sizeof(kPattern));
    FillIdealCapture(kVectorAD);
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

// ---- FR-19 / FR-20 / FR-22: StartPulseMonitor() --------------------------------------------------------------------

TEST_CASE("StartPulseMonitor configures one RMT RX channel on GPIO4 at 40 MHz", "[FR-19][FR-21][NFR-16]")
{
    ResetAll();
    REQUIRE(StartPulseMonitor());

    REQUIRE(rmt_new_rx_channel_fake.call_count == 1);
    REQUIRE(g_last_rx_config.gpio_num == RMT_PULSE_MONITOR_RX_GPIO_NUM);
    REQUIRE(g_last_rx_config.gpio_num == 4);   // jumper-wired default, not GPIO8 loopback
    REQUIRE(g_last_rx_config.resolution_hz == 40000000u);
    REQUIRE(g_last_rx_config.clk_src == RMT_CLK_SRC_DEFAULT);
    REQUIRE(g_last_rx_config.mem_block_symbols == 48);   // exactly one ESP32-C3 hardware block (M-1)
    REQUIRE(rmt_new_tx_channel_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(HarnessGetRxChannel() == kFakeRxChannel);
    REQUIRE(LogContains("pulse monitor started on GPIO 4"));
}

TEST_CASE("StartPulseMonitor registers the on_recv_done callback and creates queue, semaphores and task", "[FR-19][FR-25]")
{
    ResetAll();
    REQUIRE(StartPulseMonitor());

    REQUIRE(rmt_rx_register_event_callbacks_fake.call_count == 1);
    REQUIRE(rmt_rx_register_event_callbacks_fake.arg0_val == kFakeRxChannel);
    REQUIRE(g_last_rx_callbacks.on_recv_done == HarnessGetRxDoneCallback());

    REQUIRE(xQueueCreateStatic_fake.call_count == 1);
    REQUIRE(xQueueCreateStatic_fake.arg0_val == 1);   // 1-deep capture-done queue
    REQUIRE(xSemaphoreCreateBinaryStatic_fake.call_count == 1);
    REQUIRE(xSemaphoreCreateMutexStatic_fake.call_count == 1);
    REQUIRE(HarnessGetRxLock() != nullptr);
    REQUIRE(HarnessGetArmedSemaphore() != HarnessGetRxLock());

    REQUIRE(xTaskCreateStatic_fake.call_count == 1);
    REQUIRE(std::string(xTaskCreateStatic_fake.arg1_val) == "pulse_mon");
    REQUIRE(xTaskCreateStatic_fake.arg4_val == 2);   // lowest priority (section 7.5)
    REQUIRE(xTaskCreateStatic_fake.arg0_val == HarnessGetDecodeTaskFunction());
    REQUIRE(rmt_receive_fake.call_count == 0);       // nothing armed until led_controller asks
}

TEST_CASE("a failing StartPulseMonitor returns false, logs an Error, and later arms are silent no-ops", "[FR-20]")
{
    ResetAll();
    SECTION("RX channel creation fails")
    {
        rmt_new_rx_channel_fake.return_val = ESP_ERR_NOT_FOUND;
        REQUIRE_FALSE(StartPulseMonitor());
        REQUIRE(rmt_enable_fake.call_count == 0);
    }
    SECTION("callback registration fails")
    {
        rmt_rx_register_event_callbacks_fake.return_val = ESP_FAIL;
        REQUIRE_FALSE(StartPulseMonitor());
        REQUIRE(rmt_enable_fake.call_count == 0);
    }
    SECTION("enabling the RX channel fails")
    {
        rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
        REQUIRE_FALSE(StartPulseMonitor());
    }
    SECTION("task creation fails")
    {
        xTaskCreateStatic_fake.custom_fake = nullptr;
        xTaskCreateStatic_fake.return_val = nullptr;
        REQUIRE_FALSE(StartPulseMonitor());
        REQUIRE(rmt_disable_fake.call_count == 1);   // m-5: the enabled channel is left disabled
    }
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
    REQUIRE(HarnessGetRxChannel() == nullptr);

    TestLogReset();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));   // led_controller keeps calling it
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(LogWrite_fake.call_count == 0);
}

TEST_CASE("m-5: a task-creation failure after rmt_enable() calls rmt_disable() on the RX channel", "[FR-20]")
{
    ResetAll();
    xTaskCreateStatic_fake.custom_fake = nullptr;
    xTaskCreateStatic_fake.return_val = nullptr;
    REQUIRE_FALSE(StartPulseMonitor());

    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_disable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(HistoryIndex(Fn(rmt_enable)) < HistoryIndex(Fn(xTaskCreateStatic)));
    REQUIRE(HistoryIndex(Fn(xTaskCreateStatic)) < HistoryIndex(Fn(rmt_disable)));
    REQUIRE(LogContains("failed to create pulse monitor decode task"));
    REQUIRE(HarnessGetRxChannel() == nullptr);
}

TEST_CASE("the earlier StartPulseMonitor failures never call rmt_disable (the channel was never enabled)", "[FR-20]")
{
    ResetAll();
    SECTION("RX channel creation fails") { rmt_new_rx_channel_fake.return_val = ESP_ERR_NOT_FOUND; }
    SECTION("callback registration fails") { rmt_rx_register_event_callbacks_fake.return_val = ESP_FAIL; }
    SECTION("enabling the RX channel fails") { rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE; }
    REQUIRE_FALSE(StartPulseMonitor());
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(xTaskCreateStatic_fake.call_count == 0);
}

// ---- FR-24: ArmPulseCapture() ----------------------------------------------------------------------------------------

TEST_CASE("arm success: rmt_receive on the static buffer with the FR-22 limits", "[FR-24][FR-22][FR-23]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(rmt_receive_fake.arg0_val == kFakeRxChannel);
    REQUIRE(rmt_receive_fake.arg1_val == HarnessGetSymbolBuffer());
    REQUIRE(rmt_receive_fake.arg2_val == 576);
    REQUIRE(HarnessGetSymbolBufferBytes() == RMT_PULSE_MONITOR_BUFFER_BYTES);
    REQUIRE(g_last_receive_config.signal_range_min_ns == 50);
    REQUIRE(g_last_receive_config.signal_range_max_ns == 25000);
    REQUIRE(LogWrite_fake.call_count == 0);
}

TEST_CASE("arm success: tags the receive, then records the armed sequence and gives the armed semaphore, under the RX lock", "[FR-24][FR-31]")
{
    StartMonitor();
    const uint32_t seq_before = HarnessGetArmedSeq();
    RecordReceives();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    // Lock taken with a zero timeout (never blocks) ...
    REQUIRE(xSemaphoreTake_fake.call_count == 1);
    REQUIRE(xSemaphoreTake_fake.arg0_val == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_val == 0);
    // ... the receive is tagged with the next sequence number before it starts ...
    REQUIRE(g_capture_seq_at_receive[0] == seq_before + 1);
    REQUIRE(g_armed_seq_at_receive[0] == seq_before);   // not yet "armed"
    // ... and on success becomes the last armed sequence number.
    REQUIRE(HarnessGetArmedSeq() == seq_before + 1);
    REQUIRE(HarnessGetCaptureSeq() == seq_before + 1);
    // The capture queue is no longer reset: stale events are told apart by their tag (FR-25/FR-31).
    REQUIRE(xQueueReset_fake.call_count == 0);
    REQUIRE(xSemaphoreGive_fake.call_count == 2);
    REQUIRE(xSemaphoreGive_fake.arg0_history[0] == HarnessGetArmedSemaphore());
    REQUIRE(xSemaphoreGive_fake.arg0_history[1] == HarnessGetRxLock());   // ... and the lock released last
    REQUIRE(HistoryIndex(Fn(xSemaphoreTake)) < HistoryIndex(Fn(rmt_receive)));
    REQUIRE(HistoryIndex(Fn(rmt_receive)) < HistoryIndex(Fn(xSemaphoreGive)));
    // Non-blocking: no wait on any queue, no enable on the success path.
    REQUIRE(xQueueReceive_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 0);
}

TEST_CASE("every successful arm gets the next sequence number", "[FR-24]")
{
    StartMonitor();
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));
        REQUIRE(HarnessGetArmedSeq() == expected);
        REQUIRE(HarnessGetCaptureSeq() == expected);
    }
}

TEST_CASE("arm writes the snapshot into its own storage only after rmt_receive succeeds", "[FR-24]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));   // an earlier armed snapshot
    RecordReceives();
    ws2812_timing_t timing = kVectorAD;
    uint8_t pixels[18];
    std::memcpy(pixels, kPattern, sizeof(pixels));

    ArmPulseCapture(&timing, kAnySeq, pixels, sizeof(pixels));
    REQUIRE(SameTiming(g_timing_at_receive, kVectorA));   // still the old snapshot while rmt_receive() runs

    timing.bit0_high_ns = 1;   // the caller's buffers change after the call
    std::memset(pixels, 0xFF, sizeof(pixels));
    const ws2812_timing_t armed = HarnessGetArmedTiming();
    REQUIRE(SameTiming(armed, kVectorAD));
    REQUIRE(HarnessGetArmedPixelLength() == 18);
    REQUIRE(std::memcmp(HarnessGetArmedPixels(), kPattern, sizeof(kPattern)) == 0);
}

TEST_CASE("arm with a capture still pending: INVALID_STATE, rmt_enable rejected -> 'arm failed', no retry", "[FR-24][FR-31]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;   // previous capture still pending (run state)
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;    // ... so the channel is not in the init state
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LogContains("pulse monitor: arm failed (err=259)"));
    REQUIRE(rmt_enable_fake.call_count == 1);         // exactly one re-enable attempt ...
    REQUIRE(rmt_receive_fake.call_count == 1);        // ... rejected, so no retry
    REQUIRE(xQueueReset_fake.call_count == 0);
    REQUIRE(xSemaphoreGive_fake.call_count == 1);     // only the lock ...
    REQUIRE(xSemaphoreGive_fake.arg0_val == HarnessGetRxLock());   // ... the armed semaphore is not given
}

TEST_CASE("m-4: rmt_receive INVALID_STATE on a disabled channel -> exactly one rmt_enable and one retry", "[FR-24][NFR-12]")
{
    StartMonitor();
    RecordReceives({ESP_ERR_INVALID_STATE, ESP_OK});
    ArmPulseCapture(&kVectorAD, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(HistoryIndex(Fn(rmt_receive), 0) < HistoryIndex(Fn(rmt_enable)));
    REQUIRE(HistoryIndex(Fn(rmt_enable)) < HistoryIndex(Fn(rmt_receive), 1));
    REQUIRE(g_capture_seq_at_receive[1] == 1);        // the retry carries the same tag
    REQUIRE(rmt_receive_fake.arg1_history[1] == HarnessGetSymbolBuffer());
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);    // armed after all
    REQUIRE(HarnessGetArmedSeq() == 1);
    REQUIRE(SameTiming(HarnessGetArmedTiming(), kVectorAD));
    REQUIRE(xSemaphoreGive_fake.arg0_history[0] == HarnessGetArmedSemaphore());
}

TEST_CASE("m-4: a retry that fails again logs 'arm failed' once and does not loop", "[FR-24]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;   // enable succeeds, retry still fails
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    REQUIRE(HarnessGetArmedSeq() == 0);
}

TEST_CASE("m-4: an error other than INVALID_STATE is not followed by rmt_enable", "[FR-24]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=258)"});
}

TEST_CASE("arm failure restores the capture tag and overwrites no snapshot", "[FR-24][FR-31]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));   // seq 1: still pending
    REQUIRE(HarnessGetArmedSeq() == 1);
    FFF_RESET_HISTORY();
    RmtFakesReset();
    TestLogReset();

    RecordReceives({ESP_ERR_INVALID_STATE});
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    uint8_t other_pixels[18];
    std::memset(other_pixels, 0xFF, sizeof(other_pixels));
    ArmPulseCapture(&kVectorAD, kAnySeq, other_pixels, sizeof(other_pixels));

    REQUIRE(g_capture_seq_at_receive[0] == 2);        // tagged while the receive was attempted ...
    REQUIRE(HarnessGetCaptureSeq() == 1);             // ... restored: the pending receive keeps its tag
    REQUIRE(HarnessGetArmedSeq() == 1);
    REQUIRE(SameTiming(HarnessGetArmedTiming(), kVectorA));
    REQUIRE(std::memcmp(HarnessGetArmedPixels(), kPattern, sizeof(kPattern)) == 0);
    REQUIRE(HarnessGetArmedPixelLength() == 18);
    REQUIRE(LogContains("pulse monitor: arm failed"));

    // The pending capture's done event is still paired with its own (unchanged) snapshot.
    FillIdealCapture(kVectorA);
    TestLogReset();
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(LogContains("pulse: grb match=144/144"));
    REQUIRE(LogContains("pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800"));
}

TEST_CASE("a busy RX lock skips arming with the rx_restart_busy Warning", "[FR-24][FR-31]")
{
    StartMonitor();
    xSemaphoreTake_fake.return_val = pdFALSE;   // zero-timeout take fails: a restart holds the lock
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));

    REQUIRE(xSemaphoreTake_fake.call_count == 1);
    REQUIRE(xSemaphoreTake_fake.arg0_val == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_val == 0);   // never waits
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm skipped (reason=rx_restart_busy)"});
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(xSemaphoreGive_fake.call_count == 0);   // neither armed nor a lock it does not hold
    REQUIRE(xQueueReset_fake.call_count == 0);
    REQUIRE(HarnessGetCaptureSeq() == 0);           // no tag change without the lock
    REQUIRE(HarnessGetArmedSeq() == 0);
}

TEST_CASE("arm rejects an oversize or NULL expected pattern without touching the channel", "[FR-24]")
{
    StartMonitor();
    uint8_t big[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES + 1] = {};
    ArmPulseCapture(&kVectorA, kAnySeq, big, sizeof(big));
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LogContains("exceeds capacity"));

    ArmPulseCapture(nullptr, kAnySeq, kPattern, sizeof(kPattern));
    ArmPulseCapture(&kVectorA, kAnySeq, nullptr, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(xSemaphoreTake_fake.call_count == 0);
}

// ---- FR-25: on_recv_done ISR callback ---------------------------------------------------------------------------------

TEST_CASE("the ISR callback only overwrites the queue with the symbol count and arm sequence, and never logs", "[FR-25]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));   // seq 2
    FFF_RESET_HISTORY();
    TestLogReset();
    xQueueOverwriteFromISR_fake.custom_fake = OverwriteFromIsrWakes;

    REQUIRE(HarnessInvokeRxDone(144));   // returns "higher-priority task woken"

    REQUIRE(fff.call_history_idx == 1);
    REQUIRE(xQueueOverwriteFromISR_fake.call_count == 1);
    REQUIRE(xQueueOverwriteFromISR_fake.arg0_val == HarnessGetCaptureQueue());
    REQUIRE(xQueueSendFromISR_fake.call_count == 0);   // overwrite, so a stale event never blocks the newest
    REQUIRE(g_isr_event_count == 144);
    REQUIRE(g_isr_event_seq == 2);                     // tagged with the receive it completes
    REQUIRE(LogWrite_fake.call_count == 0);
}

TEST_CASE("the ISR callback reports no wake-up when none is needed", "[FR-25]")
{
    StartMonitor();
    REQUIRE_FALSE(HarnessInvokeRxDone(144));   // default fake leaves woken = pdFALSE
}

// ---- Decode task: FR-29, FR-30 ---------------------------------------------------------------------------------------

TEST_CASE("decode task: an idle system waits forever on the armed semaphore and logs nothing", "[FR-31]")
{
    StartMonitor();
    REQUIRE(HarnessRunDecodeTask(nullptr, 0, pdTRUE) == 0);

    REQUIRE(xSemaphoreTake_fake.call_count == 1);
    REQUIRE(xSemaphoreTake_fake.arg0_val == HarnessGetArmedSemaphore());
    REQUIRE(xSemaphoreTake_fake.arg1_val == portMAX_DELAY);
    REQUIRE(xQueueReceive_fake.call_count == 0);   // no capture timeout is running
    REQUIRE(LogWrite_fake.call_count == 0);
}

TEST_CASE("decode task: a good capture logs exactly the three section 7.6 Info lines", "[FR-29][FR-30]")
{
    StartAndArm(kVectorA);
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);

    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=400 low_ns=850; bit1 first high_ns=800 low_ns=450",
                "pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800",
                "pulse: grb match=144/144",
            });
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(rmt_disable_fake.call_count == 0);
}

TEST_CASE("decode task: the capture wait is bounded by 20 ms (2 ticks at 100 Hz) after an arm", "[FR-31]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(HarnessCaptureWaitTicks(0) == pdMS_TO_TICKS(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS));
    REQUIRE(HarnessCaptureWaitTicks(0) == 2);
    REQUIRE(xQueueReceive_fake.arg0_history[0] == HarnessGetCaptureQueue());
}

TEST_CASE("decode task: the Info lines stay within 127 characters for extreme values", "[FR-29]")
{
    // Widest numbers the RX path can report: 15-bit durations (819,175 ns) on both bit kinds.
    const ws2812_timing_t wide = {65000, 65535, 65535, 65535, 800};   // tie point 65,267.5 ns
    StartAndArm(wide);
    rmt_symbol_word_t *buffer = HarnessGetSymbolBuffer();
    for (size_t index = 0; index < 144; ++index) {
        rmt_symbol_word_t symbol{};
        symbol.level0 = 1;
        symbol.duration0 = (index % 2 == 0) ? 2610 : 0x7FFF;   // 65,250 ns -> bit 0; 819,175 ns -> bit 1
        symbol.duration1 = 0x7FFF;
        buffer[index] = symbol;
    }
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    const std::vector<std::string> lines = LinesAt(LOG_LEVEL_INFO);
    REQUIRE(lines.size() == 3);
    for (const std::string &line : lines) {
        INFO(line);
        REQUIRE(line.size() <= 127);
    }
}

TEST_CASE("decode task: one set of Info lines per captured frame", "[FR-30]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(144), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 6);
    REQUIRE(TestLogCount(LOG_LEVEL_DEBUG) == 0);   // no per-symbol detail
}

// ---- Decode task: FR-32 ----------------------------------------------------------------------------------------------

TEST_CASE("decode task: a symbol count other than 144 logs one Warning and skips decoding", "[FR-32]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(143)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_WARNING) ==
            std::vector<std::string>{"pulse monitor: symbol count mismatch (reason=symbol_count count=143)"});
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(rmt_disable_fake.call_count == 0);   // not a timeout: no restart
}

TEST_CASE("decode task: a bad symbol logs its index, the rest is still decoded and it counts as a mismatch", "[FR-32]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    HarnessGetSymbolBuffer()[5].level0 = 0;
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_WARNING) ==
            std::vector<std::string>{"pulse monitor: symbol decode error (reason=bad_symbol index=5)"});
    const std::vector<std::string> info = LinesAt(LOG_LEVEL_INFO);
    REQUIRE(info.size() == 3);
    REQUIRE(info[1] == "pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800");
    REQUIRE(info[2] == "pulse: grb match=143/144");
}

// ---- Decode task: FR-31 timeout and RX channel restart -------------------------------------------------------------

TEST_CASE("timeout: one Warning, then rmt_disable + rmt_enable under the RX lock", "[FR-31]")
{
    StartAndArm();
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_disable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeRxChannel);

    // Takes: [0] armed semaphore, [1] RX lock for GetArmedSeq() (before the wait and the Warning),
    //        [2] RX lock for the restart (blocking is fine in the decode task).
    REQUIRE(xSemaphoreTake_fake.arg0_history[0] == HarnessGetArmedSemaphore());
    REQUIRE(xSemaphoreTake_fake.arg0_history[1] == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_history[1] == portMAX_DELAY);
    REQUIRE(xSemaphoreTake_fake.arg0_history[2] == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_history[2] == portMAX_DELAY);
    const int warning_at = HistoryIndex(Fn(LogWrite));
    REQUIRE(HistoryIndex(Fn(xSemaphoreTake), 1) < HistoryIndex(Fn(xQueueReceive)));
    REQUIRE(HistoryIndex(Fn(xSemaphoreGive), 0) < HistoryIndex(Fn(xQueueReceive)));   // GetArmedSeq() released it
    // Order: Warning -> take RX lock -> check/clear the pending arm signal -> disable -> enable -> give RX lock.
    const int lock_take_at = HistoryIndex(Fn(xSemaphoreTake), 2);
    REQUIRE(warning_at < lock_take_at);
    REQUIRE(lock_take_at < HistoryIndex(Fn(uxSemaphoreGetCount)));
    REQUIRE(uxSemaphoreGetCount_fake.arg0_val == HarnessGetArmedSemaphore());
    REQUIRE(HistoryIndex(Fn(uxSemaphoreGetCount)) < HistoryIndex(Fn(rmt_disable)));
    REQUIRE(HistoryIndex(Fn(rmt_disable)) < HistoryIndex(Fn(rmt_enable)));
    REQUIRE(HistoryIndex(Fn(rmt_enable)) < HistoryIndex(Fn(xSemaphoreGive), 1));
    REQUIRE(xSemaphoreGive_fake.call_count == 2);   // GetArmedSeq() and the restart
    REQUIRE(xSemaphoreGive_fake.arg0_history[0] == HarnessGetRxLock());
    REQUIRE(xSemaphoreGive_fake.arg0_history[1] == HarnessGetRxLock());
    REQUIRE(HarnessPendingArmClears() == 0);        // nothing pending (count 0): no take
}

TEST_CASE("timeout: the restart is skipped if a newer capture is already armed", "[FR-31]")
{
    StartAndArm();   // seq 1: the capture being waited on
    // A real newer arm (seq 2) succeeds during the wait (a late done event freed the channel).
    const harness_capture_t captures[] = {{false, 0, 0, +[] {
                                               ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));
                                               uxSemaphoreGetCount_fake.return_val = 1;
                                           }}};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(HarnessGetArmedSeq() == 2);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG) == std::vector<std::string>{
                "pulse monitor: rx restart skipped, a newer capture is armed (timed_out_seq=1 armed_seq=2)"});
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(HarnessPendingArmClears() == 0);   // the newer arm's signal is left for its own wait
    REQUIRE(RxLockBalance() == 0);             // every RX-lock take (task and hook) released
}

TEST_CASE("timeout: a pending arm signal alone does not skip the restart", "[FR-31]")
{
    // The former check (uxSemaphoreGetCount > 0) treated the timed-out arm's own signal as "newer".
    StartAndArm();
    uxSemaphoreGetCount_fake.return_val = 1;   // signal pending, but s_armed_seq == the waited-on seq
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(HarnessPendingArmClears() == 1);
    REQUIRE_FALSE(LogContains("rx restart skipped"));
}

TEST_CASE("timeout: a failed rmt_enable logs one Warning and is not retried", "[FR-31]")
{
    StartAndArm();
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{
                "pulse monitor: capture timed out (reason=no_signal)",
                "pulse monitor: rx restart failed (err=259)",
            });
    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(xQueueReceive_fake.call_count == 1);      // the wait is not retried either
    REQUIRE(CountCalls(Fn(xSemaphoreGive)) == 2);     // GetArmedSeq() + restart: lock released despite the failure
    REQUIRE(RxLockBalance() == 0);                    // GetArmedSeq() + restart takes, both released
}

TEST_CASE("timeout: a failed rmt_disable skips rmt_enable and logs one Warning", "[FR-31]")
{
    StartAndArm();
    rmt_disable_fake.return_val = ESP_FAIL;
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{
                "pulse monitor: capture timed out (reason=no_signal)",
                "pulse monitor: rx restart failed (err=-1)",
            });
    REQUIRE(rmt_enable_fake.call_count == 0);
}

TEST_CASE("timeout then a good capture: measurement recovers without a reboot", "[FR-31][FR-29]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Timeout(), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);

    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 3);
    REQUIRE(LogContains("pulse: grb match=144/144"));
    REQUIRE(rmt_disable_fake.call_count == 1);
}

TEST_CASE("after a restart the next arm succeeds and a late done event of the timed-out capture is discarded", "[FR-31][FR-24]")
{
    StartAndArm();   // seq 1
    const harness_capture_t timeout[] = {Timeout()};
    HarnessRunDecodeTask(timeout, 1, pdTRUE);
    REQUIRE(rmt_enable_fake.call_count == 1);

    FFF_RESET_HISTORY();
    TestLogReset();
    ArmPulseCapture(&kVectorAD, kAnySeq, kPattern, sizeof(kPattern));   // seq 2
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);   // no "arm failed"
    REQUIRE(xQueueReset_fake.call_count == 0);       // no queue reset: the tag does the discarding
    REQUIRE(HarnessGetArmedSeq() == 2);

    // The decode task then sees the late seq-1 event first, then the seq-2 capture.
    FillIdealCapture(kVectorAD);
    TestLogReset();
    const harness_capture_t captures[] = {Arrive(144, 1), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 3);      // only the seq-2 capture is decoded
    REQUIRE(LogContains("pulse: grb match=144/144"));
}

TEST_CASE("recovery: after a restart whose rmt_enable failed, the next arm re-enables and measurement recovers", "[FR-31][FR-24]")
{
    StartAndArm();   // seq 1
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;   // restart: disable OK, enable fails
    const harness_capture_t timeout[] = {Timeout()};
    HarnessRunDecodeTask(timeout, 1, pdTRUE);
    REQUIRE(LogContains("pulse monitor: rx restart failed (err=259)"));
    REQUIRE(rmt_disable_fake.call_count == 1);        // the channel is now left disabled

    // Next frame: rmt_receive() rejects the disabled channel; ArmPulseCapture re-enables and retries.
    FFF_RESET_HISTORY();
    TestLogReset();
    RmtFakesReset();
    RecordReceives({ESP_ERR_INVALID_STATE, ESP_OK});
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);   // no "arm failed": no dead end
    REQUIRE(HarnessGetArmedSeq() == 2);

    FillIdealCapture(kVectorA);
    TestLogReset();
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=400 low_ns=850; bit1 first high_ns=800 low_ns=450",
                "pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800",
                "pulse: grb match=144/144",
            });
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
}

// ---- Sequence-tag pairing (FR-24, FR-25, FR-31) ------------------------------------------------------------------

TEST_CASE("stale event: an older tag is discarded at Debug, with no Warning, no restart, no decode, and the wait continues", "[FR-31][FR-25]")
{
    StartAndArm();   // seq 1
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));   // seq 2
    FillIdealCapture(kVectorA);
    FFF_RESET_HISTORY();
    TestLogReset();

    SECTION("stale event only: the task keeps waiting")
    {
        const harness_capture_t captures[] = {Arrive(144, 1)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(xQueueReceive_fake.call_count == 2);   // a second, fresh wait after the discard
        REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);    // no decode
    }
    SECTION("stale event, then the current one")
    {
        const harness_capture_t captures[] = {Arrive(144, 1), Arrive(144)};
        REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);
        REQUIRE(HarnessCaptureWaitTicks(1) == pdMS_TO_TICKS(RMT_PULSE_MONITOR_CAPTURE_TIMEOUT_MS));   // fresh bound
        REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 3);    // only the current capture
        REQUIRE(LogContains("pulse: grb match=144/144"));
    }
    SECTION("a stale event with a wrong symbol count is not reported either")
    {
        const harness_capture_t captures[] = {Arrive(12, 1)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE_FALSE(LogContains("symbol count mismatch"));
    }
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG) == std::vector<std::string>{"pulse monitor: stale capture discarded (seq=1)"});
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);   // no timeout Warning
    REQUIRE(rmt_disable_fake.call_count == 0);       // no restart
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(HarnessPendingArmClears() == 0);         // a stale event never clears the pending arm signal
}

TEST_CASE("race: a new arm between the ISR event and the decode copy is not paired with the old symbols", "[FR-31][FR-24][FR-25]")
{
    StartAndArm(kVectorA);   // seq 1, frame of vector A captured
    FillIdealCapture(kVectorA);
    uxSemaphoreGetCount_fake.return_val = 1;   // the new arm's signal is pending when its event arrives

    // The ISR tags the vector-A capture with seq 1; before the decode task takes the RX lock,
    // led_controller arms seq 2 for vector D (new snapshot) and its frame refills the buffer.
    const harness_capture_t captures[] = {Arrive(144, 0, ArmVectorADAndRefill), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);

    REQUIRE(HarnessGetArmedSeq() == 2);
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG) == std::vector<std::string>{"pulse monitor: stale capture discarded (seq=1)"});
    // Exactly one decode, and it pairs the vector-D symbols with the vector-D snapshot: 144/144.
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=600 low_ns=1400; bit1 first high_ns=500 low_ns=500",
                "pulse: bit0 high_ns min=600 max=600 avg=600; bit1 high_ns min=500 max=500 avg=500",
                "pulse: grb match=144/144",
            });
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(HarnessPendingArmClears() == 1);   // the matched seq-2 event clears seq 2's pending arm signal
}

TEST_CASE("race: an arm right after the decode task's copy does not change the capture being decoded", "[FR-31][FR-24]")
{
    StartAndArm(kVectorA);   // seq 1
    FillIdealCapture(kVectorA);
    // The earliest point another task can arm again is when the decode task releases the RX lock
    // after copying: overwrite the snapshot (vector D) and the capture buffer right there.
    // Skip GetArmedSeq()'s release; fire on the release that follows TakeCurrentCapture()'s copy.
    HarnessSetAfterRxLockReleaseHook(ArmVectorADAndRefill, 1);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    HarnessSetAfterRxLockReleaseHook(nullptr, 0);

    REQUIRE(HarnessGetArmedSeq() == 2);                  // the hook did arm
    REQUIRE(SameTiming(HarnessGetArmedTiming(), kVectorAD));
    // The decode used its private copies: vector-A symbols with the vector-A snapshot.
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=400 low_ns=850; bit1 first high_ns=800 low_ns=450",
                "pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800",
                "pulse: grb match=144/144",
            });
}

TEST_CASE("the decode task compares the tag and copies under the RX lock", "[FR-31]")
{
    StartAndArm();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    // Takes: [0] armed semaphore, [1] RX lock for GetArmedSeq() (before the wait),
    //        [2] RX lock for the tag compare + copy (after the event), each released before the next.
    REQUIRE(xSemaphoreTake_fake.arg0_history[0] == HarnessGetArmedSemaphore());
    REQUIRE(xSemaphoreTake_fake.arg0_history[1] == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg0_history[2] == HarnessGetRxLock());
    REQUIRE(xSemaphoreTake_fake.arg1_history[2] == portMAX_DELAY);
    REQUIRE(HistoryIndex(Fn(xSemaphoreTake), 1) < HistoryIndex(Fn(xSemaphoreGive), 0));
    REQUIRE(HistoryIndex(Fn(xSemaphoreGive), 0) < HistoryIndex(Fn(xQueueReceive)));
    REQUIRE(HistoryIndex(Fn(xQueueReceive)) < HistoryIndex(Fn(xSemaphoreTake), 2));
    REQUIRE(HistoryIndex(Fn(xSemaphoreTake), 2) < HistoryIndex(Fn(xSemaphoreGive), 1));
    REQUIRE(xSemaphoreGive_fake.call_count == 2);
    REQUIRE(xSemaphoreGive_fake.arg0_history[1] == HarnessGetRxLock());
    REQUIRE(HistoryIndex(Fn(xSemaphoreGive), 1) < HistoryIndex(Fn(LogWrite)));   // decoding happens after release
}

// ---- FR-31 regression: exactly one timeout Warning and an immediate restart after a stale event -------------------

TEST_CASE("regression: stale event, then timeout -> one Warning, immediate restart, arm signal consumed once", "[FR-31][FR-24]")
{
    StartAndArm();                                             // seq 1, its event already posted by the ISR
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));    // seq 2 armed before event 1 is consumed
    REQUIRE(HarnessGetArmedSeq() == 2);
    FreeRtosFakesReset();                                      // also clears the per-fake argument histories
    RmtFakesReset();
    TestLogReset();
    uxSemaphoreGetCount_fake.return_val = 1;                   // arm 2's signal is pending, as on target

    const harness_capture_t captures[] = {Arrive(144, 1), Timeout()};
    HarnessRunDecodeTask(captures, 2, pdTRUE);

    REQUIRE(LogContains("[L0 pulse_mon] pulse monitor: stale capture discarded (seq=1)"));
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
    REQUIRE(xQueueReceive_fake.call_count == 2);               // no third 20 ms wait for the same arm
    REQUIRE(rmt_disable_fake.call_count == 1);
    REQUIRE(rmt_disable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeRxChannel);
    const int warning_at = HistoryIndex(Fn(LogWrite), 1);      // [0] is the stale Debug line
    REQUIRE(warning_at < HistoryIndex(Fn(rmt_disable)));
    REQUIRE(HistoryIndex(Fn(rmt_disable)) < HistoryIndex(Fn(rmt_enable)));
    // Arm 2's pending signal is consumed exactly once, before rmt_disable().
    REQUIRE(HarnessPendingArmClears() == 1);
    int clear_at = -1;
    for (int occurrence = 0, index; (index = HistoryIndex(Fn(xSemaphoreTake), occurrence)) >= 0; ++occurrence) {
        if (xSemaphoreTake_fake.arg0_history[occurrence] == HarnessGetArmedSemaphore() &&
            xSemaphoreTake_fake.arg1_history[occurrence] == 0) {
            clear_at = index;
        }
    }
    REQUIRE(clear_at >= 0);
    REQUIRE(warning_at < clear_at);
    REQUIRE(clear_at < HistoryIndex(Fn(rmt_disable)));
    REQUIRE_FALSE(LogContains("rx restart skipped"));
    REQUIRE(LogContains("pulse monitor: rx channel restarted after timeout"));
}

TEST_CASE("counterpart: a third arm during the wait -> one Warning, restart skipped, newer arm kept", "[FR-31][FR-24]")
{
    StartAndArm();                                             // seq 1
    ArmPulseCapture(&kVectorA, kAnySeq, kPattern, sizeof(kPattern));    // seq 2 armed before event 1 is consumed
    FreeRtosFakesReset();                                      // also clears the per-fake argument histories
    RmtFakesReset();
    TestLogReset();
    uxSemaphoreGetCount_fake.return_val = 1;

    // Seq 3 succeeds during the wait for seq 2, which then times out.
    const harness_capture_t captures[] = {Arrive(144, 1), {false, 0, 0, +[] {
                                                               ArmPulseCapture(&kVectorAD, kAnySeq, kPattern, sizeof(kPattern));
                                                               uxSemaphoreGetCount_fake.return_val = 1;
                                                           }}};
    // Script: 2 queue waits; the third wait (for seq 3) runs the script dry and ends the loop.
    HarnessRunDecodeTask(captures, 2, pdTRUE);

    REQUIRE(HarnessGetArmedSeq() == 3);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
    REQUIRE(rmt_disable_fake.call_count == 0);
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(LogContains("[L0 pulse_mon] pulse monitor: rx restart skipped, a newer capture is armed (timed_out_seq=2 armed_seq=3)"));
    // The newer arm is not aborted: its signal stays pending and the task takes it and waits for seq 3.
    REQUIRE(HarnessPendingArmClears() == 0);
    REQUIRE(xQueueReceive_fake.call_count == 3);               // a third wait, for seq 3
    int armed_takes = 0;
    for (unsigned index = 0; index < xSemaphoreTake_fake.call_count && index < FFF_ARG_HISTORY_LEN; ++index) {
        armed_takes += (xSemaphoreTake_fake.arg0_history[index] == HarnessGetArmedSemaphore() &&
                        xSemaphoreTake_fake.arg1_history[index] == portMAX_DELAY);
    }
    REQUIRE(armed_takes == 2);                                 // the seq-2 wait, then the seq-3 wait
}

// ---- Decode rule end to end (FR-27, FR-28, section 7.6) ----------------------------------------------------------

TEST_CASE("decode task: equal commanded high times (vector AC) log line 3 as exactly 'pulse: grb match=n/a'", "[FR-28][FR-27][FR-29]")
{
    StartAndArm(kVectorAC);
    FillIdealCapture(kVectorAC);
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);

    // Lines 1-2 partition by the expected bit, so bit 1 is reported although no symbol decodes as 1.
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=500 low_ns=750; bit1 first high_ns=500 low_ns=500",
                "pulse: bit0 high_ns min=500 max=500 avg=500; bit1 high_ns min=500 max=500 avg=500",
                "pulse: grb match=n/a",
            });
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
}

TEST_CASE("decode task: vector AD (inverted highs, tuner-reachable) ideal capture logs 144/144", "[FR-27][FR-28][FR-29]")
{
    StartAndArm(kVectorAD);
    FillIdealCapture(kVectorAD);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=600 low_ns=1400; bit1 first high_ns=500 low_ns=500",
                "pulse: bit0 high_ns min=600 max=600 avg=600; bit1 high_ns min=500 max=500 avg=500",
                "pulse: grb match=144/144",
            });
}

// ---- Static review: the ISR callback does not log (FR-25) ----------------------------------------------------------

TEST_CASE("static review: HandleRxDone contains no log call and no decoding", "[FR-25]")
{
    const std::string source = ReadSource(RMT_PULSE_MONITOR_SRC);
    const size_t begin = source.find("static bool HandleRxDone(");
    REQUIRE(begin != std::string::npos);
    const size_t end = source.find("\n}\n", begin);
    REQUIRE(end != std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    REQUIRE(body.find("LOG_") == std::string::npos);
    REQUIRE(body.find("Decode") == std::string::npos);
    REQUIRE(body.find("xQueueOverwriteFromISR") != std::string::npos);
    REQUIRE(body.find("xQueueSendFromISR") == std::string::npos);
    REQUIRE(source.find("xQueueReset") == std::string::npos);   // stale events are discarded by tag instead
    for (const char *call : {"malloc(", "calloc(", "realloc(", " free("}) {
        INFO(call);
        REQUIRE(source.find(call) == std::string::npos);   // NFR-13
    }
}

// =====================================================================================================================
// T-20 (FR-34 to FR-37, NFR-17; 2026-10-03): result publication through the registered callback
// =====================================================================================================================

namespace {

struct Published {
    ws2812_measurement_t measurement;
    int rx_lock_balance;       // decode path: RX-lock takes minus gives at the moment of the call (0 = not held)
    bool rx_lock_held;         // arm path: tracked by the lock fakes below
    int info_lines_before;     // Info lines already written when the callback ran
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

// Lock fakes for the arm path: they track whether ArmPulseCapture() really holds the RX lock.
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

void RequireOneNotMeasured(uint32_t submit_seq)
{
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == submit_seq);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_NOT_MEASURED);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 0);
    REQUIRE(g_published[0].measurement.bit1_high_avg_ns == 0);
    REQUIRE_FALSE(g_published[0].rx_lock_held);
}

}  // namespace

TEST_CASE("T-20: SetPulseResultCallback stores the receiver; NULL clears it", "[T-20][FR-34]")
{
    ResetAll();
    REQUIRE(HarnessGetResultCallback() == nullptr);
    SetPulseResultCallback(RecordPublished);
    REQUIRE(HarnessGetResultCallback() == RecordPublished);
    SetPulseResultCallback(nullptr);
    REQUIRE(HarnessGetResultCallback() == nullptr);
}

TEST_CASE("T-20: a successful arm stores submit_seq with the snapshot and publishes nothing", "[T-20][FR-24][FR-35]")
{
    StartMonitor();
    ListenForResults();
    ArmPulseCapture(&kVectorA, 42, kPattern, sizeof(kPattern));
    REQUIRE(HarnessGetArmedSubmitSeq() == 42);
    REQUIRE(g_published.empty());
}

TEST_CASE("T-20: a matched 144-symbol capture publishes one done with the FR-28 averages and match", "[T-20][FR-35][FR-37]")
{
    SECTION("vector A: 144/144")
    {
        StartAndArm(kVectorA, 42);
        ListenForResults();
        FillIdealCapture(kVectorA);
        const harness_capture_t captures[] = {Arrive(144)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(g_published.size() == 1);
        const ws2812_measurement_t &m = g_published[0].measurement;
        REQUIRE(m.submit_seq == 42);
        REQUIRE(m.state == WS2812_MEASUREMENT_DONE);
        REQUIRE(m.bit0_high_avg_ns == 400);
        REQUIRE(m.bit1_high_avg_ns == 800);
        REQUIRE(m.match_count == 144);
        REQUIRE(m.match_available);
    }
    SECTION("vector AC: equal highs, match not available")
    {
        StartAndArm(kVectorAC, 43);
        ListenForResults();
        FillIdealCapture(kVectorAC);
        const harness_capture_t captures[] = {Arrive(144)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(g_published.size() == 1);
        const ws2812_measurement_t &m = g_published[0].measurement;
        REQUIRE(m.submit_seq == 43);
        REQUIRE(m.state == WS2812_MEASUREMENT_DONE);
        REQUIRE(m.bit0_high_avg_ns == 500);
        REQUIRE(m.bit1_high_avg_ns == 500);
        REQUIRE(m.match_count == 0);
        REQUIRE_FALSE(m.match_available);
    }
    SECTION("vector AD: inverted highs")
    {
        StartAndArm(kVectorAD, 44);
        ListenForResults();
        FillIdealCapture(kVectorAD);
        const harness_capture_t captures[] = {Arrive(144)};
        HarnessRunDecodeTask(captures, 1, pdTRUE);
        REQUIRE(g_published.size() == 1);
        REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 600);
        REQUIRE(g_published[0].measurement.bit1_high_avg_ns == 500);
        REQUIRE(g_published[0].measurement.match_count == 144);
    }
    REQUIRE(g_published[0].rx_lock_balance == 0);      // published after the RX lock was released
    REQUIRE(g_published[0].info_lines_before == 3);    // the three terminal lines come first and stay
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 3);
}

TEST_CASE("T-20: a capture with a bad symbol still publishes done (FR-32)", "[T-20][FR-32][FR-35]")
{
    StartAndArm(kVectorA, 45);
    ListenForResults();
    FillIdealCapture(kVectorA);
    HarnessGetSymbolBuffer()[5].level0 = 0;
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_DONE);
    REQUIRE(g_published[0].measurement.match_count == 143);
    REQUIRE(g_published[0].measurement.submit_seq == 45);
}

TEST_CASE("T-20: a symbol count mismatch publishes count_error after its Warning", "[T-20][FR-32][FR-35][FR-37]")
{
    StartAndArm(kVectorA, 46);
    ListenForResults();
    const harness_capture_t captures[] = {Arrive(143)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 46);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_COUNT_ERROR);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 0);
    REQUIRE_FALSE(g_published[0].measurement.match_available);
    REQUIRE(g_published[0].rx_lock_balance == 0);
    REQUIRE(g_published[0].warnings_before == 1);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) ==
            std::vector<std::string>{"pulse monitor: symbol count mismatch (reason=symbol_count count=143)"});
}

TEST_CASE("T-20: a timeout publishes timeout for the waited arm, after the restart released the lock", "[T-20][FR-31][FR-35][FR-37]")
{
    StartAndArm(kVectorA, 47);
    ListenForResults();
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(rmt_disable_fake.call_count == 1);         // the restart ran
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 47);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);
    REQUIRE(g_published[0].rx_lock_balance == 0);
    REQUIRE(g_published[0].warnings_before == 1);      // after the unchanged timeout Warning
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: capture timed out (reason=no_signal)"});
}

TEST_CASE("T-20: a timeout whose restart fails still publishes timeout once", "[T-20][FR-31][FR-35]")
{
    StartAndArm(kVectorA, 48);
    ListenForResults();
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    const harness_capture_t captures[] = {Timeout()};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);
    REQUIRE(g_published[0].measurement.submit_seq == 48);
}

TEST_CASE("T-20: a timeout with the restart skipped publishes timeout for the waited arm, not the newer one", "[T-20][FR-31][FR-35]")
{
    StartAndArm(kVectorA, 50);   // the arm being waited on
    ListenForResults();
    const harness_capture_t captures[] = {{false, 0, 0, +[] {
                                               ArmPulseCapture(&kVectorAD, 51, kPattern, sizeof(kPattern));
                                               uxSemaphoreGetCount_fake.return_val = 1;
                                           }}};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(rmt_disable_fake.call_count == 0);         // restart skipped
    REQUIRE(HarnessGetArmedSubmitSeq() == 51);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 50);
    REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);
}

TEST_CASE("T-20: a stale event publishes nothing for the stale arm; the newer arm gets its outcome", "[T-20][FR-31][FR-35]")
{
    StartAndArm(kVectorA, 60);                                     // arm 1
    ArmPulseCapture(&kVectorA, 61, kPattern, sizeof(kPattern));    // arm 2, before event 1 is consumed
    FillIdealCapture(kVectorA);
    FreeRtosFakesReset();
    TestLogReset();
    ListenForResults();

    SECTION("stale, then the current capture -> one done for 61")
    {
        const harness_capture_t captures[] = {Arrive(144, 1), Arrive(144)};
        HarnessRunDecodeTask(captures, 2, pdTRUE);
        REQUIRE(g_published.size() == 1);
        REQUIRE(g_published[0].measurement.submit_seq == 61);
        REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_DONE);
    }
    SECTION("stale, then a timeout -> one timeout for 61")
    {
        uxSemaphoreGetCount_fake.return_val = 1;
        const harness_capture_t captures[] = {Arrive(144, 1), Timeout()};
        HarnessRunDecodeTask(captures, 2, pdTRUE);
        REQUIRE(g_published.size() == 1);
        REQUIRE(g_published[0].measurement.submit_seq == 61);
        REQUIRE(g_published[0].measurement.state == WS2812_MEASUREMENT_TIMEOUT);
    }
    for (const Published &published : g_published) {
        REQUIRE(published.measurement.submit_seq != 60);
    }
}

TEST_CASE("T-20: the race-discarded old capture publishes nothing; the decoded one carries the new number", "[T-20][FR-31][FR-35]")
{
    StartAndArm(kVectorA, 70);
    FillIdealCapture(kVectorA);
    ListenForResults();
    uxSemaphoreGetCount_fake.return_val = 1;
    const harness_capture_t captures[] = {Arrive(144, 0, +[] {
                                              ArmPulseCapture(&kVectorAD, 71, kPattern, sizeof(kPattern));
                                              FillIdealCapture(kVectorAD);
                                          }),
                                          Arrive(144)};
    HarnessRunDecodeTask(captures, 2, pdTRUE);
    REQUIRE(g_published.size() == 1);
    REQUIRE(g_published[0].measurement.submit_seq == 71);
    REQUIRE(g_published[0].measurement.bit0_high_avg_ns == 600);
}

TEST_CASE("T-20: ArmPulseCapture publishes not_measured on every return without arming, lock not held", "[T-20][FR-24][FR-36][FR-37]")
{
    SECTION("monitor not started")
    {
        ResetAll();
        ListenForResults();
        ArmPulseCapture(&kVectorA, 80, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(80);
    }
    SECTION("NULL timing")
    {
        StartMonitor();
        ListenForResults();
        ArmPulseCapture(nullptr, 81, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(81);
    }
    SECTION("NULL pixel buffer")
    {
        StartMonitor();
        ListenForResults();
        ArmPulseCapture(&kVectorA, 82, nullptr, sizeof(kPattern));
        RequireOneNotMeasured(82);
    }
    SECTION("pixel length above capacity")
    {
        StartMonitor();
        ListenForResults();
        uint8_t big[RMT_PULSE_MONITOR_EXPECTED_PIXEL_MAX_BYTES + 1] = {};
        ArmPulseCapture(&kVectorA, 83, big, sizeof(big));
        RequireOneNotMeasured(83);
        REQUIRE(LogContains("exceeds capacity"));
    }
    SECTION("RX lock busy (rx_restart_busy)")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(true);
        ArmPulseCapture(&kVectorA, 84, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(84);
        REQUIRE(LogContains("pulse monitor: arm skipped (reason=rx_restart_busy)"));
        REQUIRE(g_published[0].warnings_before == 1);
    }
    SECTION("rmt_receive fails (arm failed)")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
        ArmPulseCapture(&kVectorA, 85, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(85);
        REQUIRE(LogContains("pulse monitor: arm failed (err=258)"));
        REQUIRE(g_published[0].warnings_before == 1);
    }
    SECTION("re-enable and retry both fail")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
        ArmPulseCapture(&kVectorA, 86, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(86);
        REQUIRE(rmt_receive_fake.call_count == 2);
    }
    SECTION("previous capture pending: INVALID_STATE, rmt_enable rejected")
    {
        StartMonitor();
        ListenForResults();
        TrackRxLock(false);
        rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;
        rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
        ArmPulseCapture(&kVectorA, 87, kPattern, sizeof(kPattern));
        RequireOneNotMeasured(87);
    }
    xSemaphoreTake_fake.custom_fake = nullptr;
    xSemaphoreGive_fake.custom_fake = nullptr;
}

TEST_CASE("T-20: a successful retry after re-enable publishes nothing from the arm path", "[T-20][FR-24][FR-36]")
{
    StartMonitor();
    ListenForResults();
    RecordReceives({ESP_ERR_INVALID_STATE, ESP_OK});
    ArmPulseCapture(&kVectorA, 88, kPattern, sizeof(kPattern));
    REQUIRE(g_published.empty());
    REQUIRE(HarnessGetArmedSubmitSeq() == 88);
}

TEST_CASE("T-20: no callback registered -> every path runs without a crash and logs the same lines", "[T-20][FR-37]")
{
    std::string with_callback_log;
    std::string without_callback_log;
    for (bool with_callback : {true, false}) {
        StartAndArm(kVectorA, 90);
        if (with_callback) {
            ListenForResults();
        } else {
            SetPulseResultCallback(nullptr);
        }
        FillIdealCapture(kVectorA);
        const harness_capture_t captures[] = {Arrive(144), Timeout()};
        HarnessRunDecodeTask(captures, 1, pdTRUE);           // done
        ArmPulseCapture(&kVectorA, 91, kPattern, sizeof(kPattern));
        HarnessRunDecodeTask(captures + 1, 1, pdTRUE);       // timeout
        ArmPulseCapture(&kVectorA, 92, kPattern, 99);        // not_measured (oversize)
        const harness_capture_t short_capture[] = {Arrive(12)};
        ArmPulseCapture(&kVectorA, 93, kPattern, sizeof(kPattern));
        HarnessRunDecodeTask(short_capture, 1, pdTRUE);      // count_error
        (with_callback ? with_callback_log : without_callback_log) = Log();
    }
    REQUIRE(g_published.size() == 4);
    // FR-37 / SPEC-003 FR-29: publication adds, changes or removes no terminal line.
    REQUIRE(with_callback_log == without_callback_log);
}

TEST_CASE("T-20: the ISR callback never publishes", "[T-20][FR-25][FR-37]")
{
    StartAndArm(kVectorA, 95);
    ListenForResults();
    REQUIRE(HarnessInvokeRxDone(144) == false);
    REQUIRE(HarnessInvokeRxDone(12) == false);
    REQUIRE(g_published.empty());
}

TEST_CASE("terminal output unchanged: the three section 7.6 pulse lines byte for byte", "[T-21][FR-29][FR-37][SPEC-003][FR-29]")
{
    StartAndArm(kVectorA, 96);
    ListenForResults();
    FillIdealCapture(kVectorA);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    REQUIRE(Log() ==
            "[L1 pulse_mon] pulse: bit0 first high_ns=400 low_ns=850; bit1 first high_ns=800 low_ns=450\n"
            "[L1 pulse_mon] pulse: bit0 high_ns min=400 max=400 avg=400; bit1 high_ns min=800 max=800 avg=800\n"
            "[L1 pulse_mon] pulse: grb match=144/144\n");
}

TEST_CASE("static review: publication is outside the ISR and the component knows no receiver", "[T-20][FR-25][FR-34][FR-37][NFR-17]")
{
    const std::string source = ReadSource(RMT_PULSE_MONITOR_SRC);
    const size_t begin = source.find("static bool HandleRxDone(");
    const size_t end = source.find("\n}\n", begin);
    const std::string isr = source.substr(begin, end - begin);
    REQUIRE(isr.find("PublishPulseResult") == std::string::npos);
    REQUIRE(isr.find("s_result_cb") == std::string::npos);
    for (const char *include : {"http_portal.h", "provisioning.h", "led_controller.h"}) {
        INFO(include);
        REQUIRE(source.find(include) == std::string::npos);
    }
    // Every callback invocation goes through PublishPulseResult(); that function is the only reader of s_result_cb.
    size_t readers = 0;
    for (size_t at = source.find("s_result_cb"); at != std::string::npos; at = source.find("s_result_cb", at + 1)) {
        ++readers;
    }
    REQUIRE(readers == 3);   // declaration, SetPulseResultCallback() write, PublishPulseResult() read
}
