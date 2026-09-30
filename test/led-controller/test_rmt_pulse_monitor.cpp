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
const ws2812_timing_t kVectorB = {100, 800, 100, 800, 50};
const ws2812_timing_t kVectorD = {1200, 2000, 1000, 2000, 800};
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
void StartAndArm(const ws2812_timing_t &timing = kVectorA)
{
    StartMonitor();
    ArmPulseCapture(&timing, kPattern, sizeof(kPattern));
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
void ArmVectorDAndRefill()
{
    ArmPulseCapture(&kVectorD, kPattern, sizeof(kPattern));
    FillIdealCapture(kVectorD);
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));   // led_controller keeps calling it
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

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
        ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));
        REQUIRE(HarnessGetArmedSeq() == expected);
        REQUIRE(HarnessGetCaptureSeq() == expected);
    }
}

TEST_CASE("arm writes the snapshot into its own storage only after rmt_receive succeeds", "[FR-24]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));   // an earlier armed snapshot
    RecordReceives();
    ws2812_timing_t timing = kVectorD;
    uint8_t pixels[18];
    std::memcpy(pixels, kPattern, sizeof(pixels));

    ArmPulseCapture(&timing, pixels, sizeof(pixels));
    REQUIRE(SameTiming(g_timing_at_receive, kVectorA));   // still the old snapshot while rmt_receive() runs

    timing.bit0_high_ns = 1;   // the caller's buffers change after the call
    std::memset(pixels, 0xFF, sizeof(pixels));
    const ws2812_timing_t armed = HarnessGetArmedTiming();
    REQUIRE(SameTiming(armed, kVectorD));
    REQUIRE(HarnessGetArmedPixelLength() == 18);
    REQUIRE(std::memcmp(HarnessGetArmedPixels(), kPattern, sizeof(kPattern)) == 0);
}

TEST_CASE("arm with a capture still pending: INVALID_STATE, rmt_enable rejected -> 'arm failed', no retry", "[FR-24][FR-31]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;   // previous capture still pending (run state)
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;    // ... so the channel is not in the init state
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

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
    ArmPulseCapture(&kVectorD, kPattern, sizeof(kPattern));

    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeRxChannel);
    REQUIRE(HistoryIndex(Fn(rmt_receive), 0) < HistoryIndex(Fn(rmt_enable)));
    REQUIRE(HistoryIndex(Fn(rmt_enable)) < HistoryIndex(Fn(rmt_receive), 1));
    REQUIRE(g_capture_seq_at_receive[1] == 1);        // the retry carries the same tag
    REQUIRE(rmt_receive_fake.arg1_history[1] == HarnessGetSymbolBuffer());
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);    // armed after all
    REQUIRE(HarnessGetArmedSeq() == 1);
    REQUIRE(SameTiming(HarnessGetArmedTiming(), kVectorD));
    REQUIRE(xSemaphoreGive_fake.arg0_history[0] == HarnessGetArmedSemaphore());
}

TEST_CASE("m-4: a retry that fails again logs 'arm failed' once and does not loop", "[FR-24]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_STATE;   // enable succeeds, retry still fails
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_receive_fake.call_count == 2);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=259)"});
    REQUIRE(HarnessGetArmedSeq() == 0);
}

TEST_CASE("m-4: an error other than INVALID_STATE is not followed by rmt_enable", "[FR-24]")
{
    StartMonitor();
    rmt_receive_fake.return_val = ESP_ERR_INVALID_ARG;
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(rmt_receive_fake.call_count == 1);
    REQUIRE(LinesAt(LOG_LEVEL_WARNING) == std::vector<std::string>{"pulse monitor: arm failed (err=258)"});
}

TEST_CASE("arm failure restores the capture tag and overwrites no snapshot", "[FR-24][FR-31]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));   // seq 1: still pending
    REQUIRE(HarnessGetArmedSeq() == 1);
    FFF_RESET_HISTORY();
    RmtFakesReset();
    TestLogReset();

    RecordReceives({ESP_ERR_INVALID_STATE});
    rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
    uint8_t other_pixels[18];
    std::memset(other_pixels, 0xFF, sizeof(other_pixels));
    ArmPulseCapture(&kVectorD, other_pixels, sizeof(other_pixels));

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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));

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
    ArmPulseCapture(&kVectorA, big, sizeof(big));
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LogContains("exceeds capacity"));

    ArmPulseCapture(nullptr, kPattern, sizeof(kPattern));
    ArmPulseCapture(&kVectorA, nullptr, sizeof(kPattern));
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(xSemaphoreTake_fake.call_count == 0);
}

// ---- FR-25: on_recv_done ISR callback ---------------------------------------------------------------------------------

TEST_CASE("the ISR callback only overwrites the queue with the symbol count and arm sequence, and never logs", "[FR-25]")
{
    StartMonitor();
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));   // seq 2
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
                                               ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));
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
    ArmPulseCapture(&kVectorD, kPattern, sizeof(kPattern));   // seq 2
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);   // no "arm failed"
    REQUIRE(xQueueReset_fake.call_count == 0);       // no queue reset: the tag does the discarding
    REQUIRE(HarnessGetArmedSeq() == 2);

    // The decode task then sees the late seq-1 event first, then the seq-2 capture.
    FillIdealCapture(kVectorD);
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));   // seq 2
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
    const harness_capture_t captures[] = {Arrive(144, 0, ArmVectorDAndRefill), Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 2, pdTRUE) == 2);

    REQUIRE(HarnessGetArmedSeq() == 2);
    REQUIRE(LinesAt(LOG_LEVEL_DEBUG) == std::vector<std::string>{"pulse monitor: stale capture discarded (seq=1)"});
    // Exactly one decode, and it pairs the vector-D symbols with the vector-D snapshot: 144/144.
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=1200 low_ns=800; bit1 first high_ns=1000 low_ns=1000",
                "pulse: bit0 high_ns min=1200 max=1200 avg=1200; bit1 high_ns min=1000 max=1000 avg=1000",
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
    HarnessSetAfterRxLockReleaseHook(ArmVectorDAndRefill, 1);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);
    HarnessSetAfterRxLockReleaseHook(nullptr, 0);

    REQUIRE(HarnessGetArmedSeq() == 2);                  // the hook did arm
    REQUIRE(SameTiming(HarnessGetArmedTiming(), kVectorD));
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));    // seq 2 armed before event 1 is consumed
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
    ArmPulseCapture(&kVectorA, kPattern, sizeof(kPattern));    // seq 2 armed before event 1 is consumed
    FreeRtosFakesReset();                                      // also clears the per-fake argument histories
    RmtFakesReset();
    TestLogReset();
    uxSemaphoreGetCount_fake.return_val = 1;

    // Seq 3 succeeds during the wait for seq 2, which then times out.
    const harness_capture_t captures[] = {Arrive(144, 1), {false, 0, 0, +[] {
                                                               ArmPulseCapture(&kVectorD, kPattern, sizeof(kPattern));
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

TEST_CASE("decode task: equal commanded high times log line 3 as exactly 'pulse: grb match=n/a'", "[FR-28][FR-27][FR-29]")
{
    StartAndArm(kVectorB);
    FillIdealCapture(kVectorB);
    const harness_capture_t captures[] = {Arrive(144)};
    REQUIRE(HarnessRunDecodeTask(captures, 1, pdTRUE) == 1);

    // Lines 1-2 partition by the expected bit, so bit 1 is reported although no symbol decodes as 1.
    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=100 low_ns=700; bit1 first high_ns=100 low_ns=700",
                "pulse: bit0 high_ns min=100 max=100 avg=100; bit1 high_ns min=100 max=100 avg=100",
                "pulse: grb match=n/a",
            });
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
}

TEST_CASE("decode task: vector D (inverted timing) ideal capture logs 144/144", "[FR-27][FR-28][FR-29]")
{
    StartAndArm(kVectorD);
    FillIdealCapture(kVectorD);
    const harness_capture_t captures[] = {Arrive(144)};
    HarnessRunDecodeTask(captures, 1, pdTRUE);

    REQUIRE(LinesAt(LOG_LEVEL_INFO) == std::vector<std::string>{
                "pulse: bit0 first high_ns=1200 low_ns=800; bit1 first high_ns=1000 low_ns=1000",
                "pulse: bit0 high_ns min=1200 max=1200 avg=1200; bit1 high_ns min=1000 max=1000 avg=1000",
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
