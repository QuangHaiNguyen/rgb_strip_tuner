/**
 * @file test_led_controller.cpp
 * @brief Host tests for led_controller.c through a harness that compiles the file (SPEC-004
 *        T-3 FR-10/FR-11, T-6 FR-7/FR-17, T-7 FR-16, and the host-testable parts of FR-1, FR-2,
 *        FR-8, FR-13, FR-18, FR-24).
 *
 * Every RMT call is an FFF fake (mocks/rmt_fakes.c), FreeRTOS is FFF-faked
 * (mocks/freertos_fakes.c) and ArmPulseCapture() is an FFF fake (mocks/pulse_monitor_fakes.c).
 * The driver task's endless loop is run by HarnessRunDriverTask() with a scripted queue.
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
#include "led_controller.h"
#include "led_controller_harness.h"
#include "log_fake_access.h"
#include "log_fakes.h"
#include "pulse_monitor_fakes.h"
#include "rmt_fakes.h"
}

namespace {

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};
const ws2812_timing_t kVectorB = {100, 800, 100, 800, 50};
const ws2812_timing_t kVectorC = {1075, 1200, 1100, 1200, 280};
const ws2812_timing_t kVectorD = {1200, 2000, 1000, 2000, 800};   // pure symbol-builder input only (fails V4)
// 2026-10-03: vectors reachable through /tuner after SPEC-003 V4. X replaces D on every driver/queue path.
const ws2812_timing_t kVectorX = {1000, 2000, 1200, 2000, 800};
const ws2812_timing_t kVectorAC = {500, 1250, 500, 1000, 280};
const ws2812_timing_t kVectorAD = {600, 2000, 500, 1000, 280};

const uint8_t kPattern[LED_CONTROLLER_PIXEL_BYTES] = {
    0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x20,
};

struct SymbolValues {
    uint32_t duration0, level0, duration1, level1;
};

// rmt_transmit() gets a stack address for its config: keep a copy of the last one.
rmt_transmit_config_t g_last_transmit_config;
esp_err_t TransmitCopiesConfig(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t,
                               const rmt_transmit_config_t *config)
{
    g_last_transmit_config = *config;
    return rmt_transmit_fake.return_val;
}

SymbolValues Values(const rmt_symbol_word_t &symbol)
{
    return {symbol.duration0, symbol.level0, symbol.duration1, symbol.level1};
}

bool operator==(const SymbolValues &a, const SymbolValues &b)
{
    return a.duration0 == b.duration0 && a.level0 == b.level0 && a.duration1 == b.duration1 && a.level1 == b.level1;
}

bool SameTiming(const ws2812_timing_t &a, const ws2812_timing_t &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void ResetAll()
{
    TestLogReset();   // also resets FFF's call history
    FreeRtosFakesReset();
    RmtFakesReset();
    PulseMonitorFakesReset();
    HarnessResetLedController();
    FFF_RESET_HISTORY();
}

/** Start the controller with every fake succeeding, then forget the start-up calls. */
void StartController()
{
    ResetAll();
    REQUIRE(StartLedController());
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    PulseMonitorFakesReset();
    FFF_RESET_HISTORY();
}

/** Position of the n-th call of @p function in FFF's global call history, or -1. */
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

bool LogContains(const char *text) { return std::string(TestLogText()).find(text) != std::string::npos; }

// Single-slot model of the 1-deep timing queue: xQueueOverwrite() replaces the slot (item: led_request_t, FR-7).
led_request_t g_slot;
int g_slot_writes;

BaseType_t OverwriteSlot(QueueHandle_t queue, const void *item)
{
    (void)queue;
    std::memcpy(&g_slot, item, sizeof(g_slot));
    ++g_slot_writes;
    return pdPASS;
}

// Stub sub-encoders for the FR-13 frame encoder: scripted symbol counts and states.
struct EncodeStep {
    size_t symbols;
    int state;
};
std::vector<EncodeStep> g_bytes_steps;
std::vector<EncodeStep> g_copy_steps;
size_t g_bytes_calls;
size_t g_copy_calls;
const void *g_copy_data;
size_t g_copy_size;
const void *g_bytes_data;
size_t g_bytes_size;

size_t BytesEncode(rmt_encoder_t *, rmt_channel_handle_t, const void *data, size_t size, rmt_encode_state_t *state)
{
    const EncodeStep step = g_bytes_steps.at(g_bytes_calls++);
    g_bytes_data = data;
    g_bytes_size = size;
    *state = static_cast<rmt_encode_state_t>(step.state);
    return step.symbols;
}

size_t CopyEncode(rmt_encoder_t *, rmt_channel_handle_t, const void *data, size_t size, rmt_encode_state_t *state)
{
    const EncodeStep step = g_copy_steps.at(g_copy_calls++);
    g_copy_data = data;
    g_copy_size = size;
    *state = static_cast<rmt_encode_state_t>(step.state);
    return step.symbols;
}

void SetUpFrameEncoder(const ws2812_timing_t &timing)
{
    ResetAll();
    REQUIRE(HarnessInitEncoder(&timing));
    g_fake_bytes_encoder.encode = BytesEncode;
    g_fake_copy_encoder.encode = CopyEncode;
    g_bytes_steps.clear();
    g_copy_steps.clear();
    g_bytes_calls = g_copy_calls = 0;
    g_copy_data = g_bytes_data = nullptr;
    g_copy_size = g_bytes_size = 0;
}

size_t Encode(rmt_encode_state_t &state)
{
    rmt_encoder_t *encoder = HarnessGetFrameEncoder();
    return encoder->encode(encoder, kFakeTxChannel, kPattern, sizeof(kPattern), &state);
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

// ---- T-3: bit/reset symbol construction (FR-10, FR-11) -----------------------------------------------------------

TEST_CASE("symbols for the SPEC-003 defaults match the section 7.2 worked example", "[T-3][FR-10][FR-11]")
{
    const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorA);
    REQUIRE(Values(config.bit0) == SymbolValues{16, 1, 34, 0});
    REQUIRE(Values(config.bit1) == SymbolValues{32, 1, 18, 0});
    REQUIRE(config.flags.msb_first == 1);
    REQUIRE(Values(HarnessBuildResetSymbol(&kVectorA)) == SymbolValues{11200, 0, 0, 0});
}

TEST_CASE("symbols for SPEC-003 vector C and the pure inputs B and D", "[T-3][FR-10][FR-11]")
{
    SECTION("B: minimum edge")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorB);
        REQUIRE(Values(config.bit0) == SymbolValues{4, 1, 28, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{4, 1, 28, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorB)) == SymbolValues{2000, 0, 0, 0});
    }
    SECTION("C: 100 ns low-time edge")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorC);
        REQUIRE(Values(config.bit0) == SymbolValues{43, 1, 5, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{44, 1, 4, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorC)) == SymbolValues{11200, 0, 0, 0});
    }
    SECTION("D: maximum edge, 800 us reset fits one 15-bit field")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorD);
        REQUIRE(Values(config.bit0) == SymbolValues{48, 1, 32, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{40, 1, 40, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorD)) == SymbolValues{32000, 0, 0, 0});
    }
}

TEST_CASE("symbols for SPEC-003 vectors X, AC and AD (tuner-reachable after V4)", "[T-3][FR-10][FR-11]")
{
    SECTION("X: maximum edge, 800 us reset fits one 15-bit field")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorX);
        REQUIRE(Values(config.bit0) == SymbolValues{40, 1, 40, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{48, 1, 32, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorX)) == SymbolValues{32000, 0, 0, 0});
    }
    SECTION("AC: equal highs, shorter bit-1 period")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorAC);
        REQUIRE(Values(config.bit0) == SymbolValues{20, 1, 30, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{20, 1, 20, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorAC)) == SymbolValues{11200, 0, 0, 0});
    }
    SECTION("AD: inverted highs, valid duty order")
    {
        const rmt_bytes_encoder_config_t config = HarnessBuildBytesEncoderConfig(&kVectorAD);
        REQUIRE(Values(config.bit0) == SymbolValues{24, 1, 56, 0});
        REQUIRE(Values(config.bit1) == SymbolValues{20, 1, 20, 0});
        REQUIRE(Values(HarnessBuildResetSymbol(&kVectorAD)) == SymbolValues{11200, 0, 0, 0});
    }
}

TEST_CASE("the symbol builders are pure: no driver call", "[T-3][NFR-10]")
{
    ResetAll();
    (void)HarnessBuildBytesEncoderConfig(&kVectorD);
    (void)HarnessBuildResetSymbol(&kVectorD);
    REQUIRE(fff.call_history_idx == 0);
}

// ---- FR-13: one frame = 144 data symbols from the bytes encoder + 1 reset symbol from the copy encoder ------------

TEST_CASE("the frame encoder emits 144 data symbols then the reset symbol (145 total)", "[FR-13][FR-11]")
{
    SetUpFrameEncoder(kVectorA);
    g_bytes_steps = {{144, RMT_ENCODING_COMPLETE}};
    g_copy_steps = {{1, RMT_ENCODING_COMPLETE}};

    rmt_encode_state_t state = RMT_ENCODING_RESET;
    REQUIRE(Encode(state) == 145);
    REQUIRE(state == RMT_ENCODING_COMPLETE);
    REQUIRE(g_bytes_data == kPattern);
    REQUIRE(g_bytes_size == LED_CONTROLLER_PIXEL_BYTES);
    REQUIRE(g_copy_data == HarnessGetEncoderResetSymbol());      // the FR-11 reset symbol, once
    REQUIRE(g_copy_size == sizeof(rmt_symbol_word_t));
    REQUIRE(Values(*HarnessGetEncoderResetSymbol()) == SymbolValues{11200, 0, 0, 0});
}

TEST_CASE("the frame encoder resumes after the RMT memory fills up", "[FR-13]")
{
    SetUpFrameEncoder(kVectorA);
    g_bytes_steps = {{48, RMT_ENCODING_MEM_FULL}, {96, RMT_ENCODING_COMPLETE}};   // one 48-symbol block
    g_copy_steps = {{0, RMT_ENCODING_MEM_FULL}, {1, RMT_ENCODING_COMPLETE}};

    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t total = Encode(state);
    REQUIRE(state == RMT_ENCODING_MEM_FULL);   // first ISR refill: data not finished, reset not started
    REQUIRE(g_copy_calls == 0);

    total += Encode(state);                    // data finishes; reset does not fit yet
    REQUIRE(state == RMT_ENCODING_MEM_FULL);
    REQUIRE(g_copy_calls == 1);

    total += Encode(state);                    // only the reset symbol is left
    REQUIRE(state == RMT_ENCODING_COMPLETE);
    REQUIRE(g_bytes_calls == 2);               // the data was not encoded twice
    REQUIRE(g_copy_calls == 2);
    REQUIRE(total == 145);

    g_bytes_steps.push_back({144, RMT_ENCODING_COMPLETE});   // the next frame starts with data again
    g_copy_steps.push_back({1, RMT_ENCODING_COMPLETE});
    REQUIRE(Encode(state) == 145);
}

TEST_CASE("resetting the frame encoder resets both sub-encoders and restarts with data", "[FR-13]")
{
    SetUpFrameEncoder(kVectorA);
    g_bytes_steps = {{144, RMT_ENCODING_COMPLETE}, {144, RMT_ENCODING_COMPLETE}};
    g_copy_steps = {{0, RMT_ENCODING_MEM_FULL}, {1, RMT_ENCODING_COMPLETE}};
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    (void)Encode(state);   // stops in the reset stage

    rmt_encoder_t *encoder = HarnessGetFrameEncoder();
    REQUIRE(encoder->reset(encoder) == ESP_OK);
    REQUIRE(rmt_encoder_reset_fake.call_count == 2);
    REQUIRE(rmt_encoder_reset_fake.arg0_history[0] == &g_fake_bytes_encoder);
    REQUIRE(rmt_encoder_reset_fake.arg0_history[1] == &g_fake_copy_encoder);

    REQUIRE(Encode(state) == 145);
    REQUIRE(g_bytes_calls == 2);
}

// ---- FR-1 / FR-2: StartLedController() -----------------------------------------------------------------------------

TEST_CASE("StartLedController configures one RMT TX channel on GPIO8 at 40 MHz", "[FR-1][NFR-16]")
{
    ResetAll();
    REQUIRE(StartLedController());

    REQUIRE(rmt_new_tx_channel_fake.call_count == 1);
    REQUIRE(g_last_tx_config.gpio_num == LED_CONTROLLER_GPIO_NUM);
    REQUIRE(g_last_tx_config.gpio_num == 8);
    REQUIRE(g_last_tx_config.resolution_hz == 40000000u);
    REQUIRE(g_last_tx_config.clk_src == RMT_CLK_SRC_DEFAULT);
    REQUIRE(g_last_tx_config.trans_queue_depth == 1);
    REQUIRE(g_last_tx_config.flags.with_dma == 0);   // no DMA backend on ESP32-C3
    REQUIRE(g_last_tx_config.mem_block_symbols == 48);   // exactly one ESP32-C3 hardware block (M-1)
    REQUIRE(rmt_enable_fake.call_count == 1);
    REQUIRE(rmt_enable_fake.arg0_val == kFakeTxChannel);
    REQUIRE(HarnessGetTxChannel() == kFakeTxChannel);
    REQUIRE(rmt_new_rx_channel_fake.call_count == 0);
}

TEST_CASE("StartLedController builds the encoders from the SPEC-003 defaults", "[FR-1][FR-10][FR-11]")
{
    ResetAll();
    REQUIRE(StartLedController());

    REQUIRE(rmt_new_bytes_encoder_fake.call_count == 1);
    REQUIRE(Values(g_last_bytes_config.bit0) == SymbolValues{16, 1, 34, 0});
    REQUIRE(Values(g_last_bytes_config.bit1) == SymbolValues{32, 1, 18, 0});
    REQUIRE(g_last_bytes_config.flags.msb_first == 1);
    REQUIRE(rmt_new_copy_encoder_fake.call_count == 1);
    REQUIRE(Values(*HarnessGetEncoderResetSymbol()) == SymbolValues{11200, 0, 0, 0});
}

TEST_CASE("StartLedController creates the 1-deep queue and the task, then queues the boot frame", "[FR-1][FR-7]")
{
    ResetAll();
    REQUIRE(StartLedController());

    REQUIRE(xQueueCreateStatic_fake.call_count == 1);
    REQUIRE(xQueueCreateStatic_fake.arg0_val == 1);                          // length 1
    // Changed 2026-10-03: the item is led_request_t (timing + submit_seq, 16 bytes); was ws2812_timing_t (10 bytes).
    REQUIRE(xQueueCreateStatic_fake.arg1_val == sizeof(led_request_t));
    REQUIRE(sizeof(led_request_t) == 16);
    REQUIRE(xTaskCreateStatic_fake.call_count == 1);
    REQUIRE(std::string(xTaskCreateStatic_fake.arg1_val) == "led_ctrl");
    REQUIRE(xTaskCreateStatic_fake.arg4_val == 4);                           // section 7.4 priority
    REQUIRE(xTaskCreateStatic_fake.arg0_val == HarnessGetDriverTaskFunction());

    // FR-1d: the boot frame is handed to the driver task with the defaults (never transmitted here).
    REQUIRE(xQueueOverwrite_fake.call_count == 1);
    REQUIRE(xQueueOverwrite_fake.arg0_val == HarnessGetTimingQueue());
    REQUIRE(rmt_transmit_fake.call_count == 0);
    REQUIRE(HistoryIndex(Fn(xTaskCreateStatic)) < HistoryIndex(Fn(xQueueOverwrite)));

    // ... and StartLedController() waits (bounded) for the task to confirm it before returning.
    REQUIRE(xSemaphoreTake_fake.call_count == 1);
    REQUIRE(xSemaphoreTake_fake.arg0_val == HarnessGetBootDoneSemaphore());
    REQUIRE(xSemaphoreTake_fake.arg1_val != portMAX_DELAY);
    REQUIRE(HistoryIndex(Fn(xQueueOverwrite)) < HistoryIndex(Fn(xSemaphoreTake)));
    REQUIRE(LogContains("led_controller started on GPIO 8"));
}

TEST_CASE("StartLedController queues exactly the SPEC-003 default timing for the boot frame", "[FR-1]")
{
    ResetAll();
    xQueueOverwrite_fake.custom_fake = OverwriteSlot;
    g_slot_writes = 0;
    REQUIRE(StartLedController());
    REQUIRE(g_slot_writes == 1);
    REQUIRE(SameTiming(g_slot.timing, kVectorA));
    REQUIRE(g_slot.submit_seq == 0);   // FR-7 (2026-10-03): the boot frame uses submit_seq 0
}

TEST_CASE("an unconfirmed boot frame only logs a Warning; start still succeeds", "[FR-1]")
{
    ResetAll();
    xSemaphoreTake_fake.return_val = pdFALSE;
    REQUIRE(StartLedController());
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(LogContains("boot frame not confirmed"));
}

TEST_CASE("a failing init step returns false, logs an Error and leaves the strip undriven", "[FR-2]")
{
    ResetAll();
    SECTION("RMT TX channel creation fails")
    {
        rmt_new_tx_channel_fake.return_val = ESP_FAIL;
        REQUIRE_FALSE(StartLedController());
        REQUIRE(rmt_new_bytes_encoder_fake.call_count == 0);
    }
    SECTION("bytes encoder creation fails")
    {
        rmt_new_bytes_encoder_fake.return_val = ESP_ERR_NO_MEM;
        REQUIRE_FALSE(StartLedController());
    }
    SECTION("copy (reset) encoder creation fails")
    {
        rmt_new_copy_encoder_fake.return_val = ESP_ERR_NO_MEM;
        REQUIRE_FALSE(StartLedController());
    }
    SECTION("enabling the channel fails")
    {
        rmt_enable_fake.return_val = ESP_ERR_INVALID_STATE;
        REQUIRE_FALSE(StartLedController());
    }
    SECTION("task creation fails")
    {
        xTaskCreateStatic_fake.custom_fake = nullptr;
        xTaskCreateStatic_fake.return_val = nullptr;
        REQUIRE_FALSE(StartLedController());
    }
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
    REQUIRE(xQueueOverwrite_fake.call_count == 0);   // no boot frame queued
    REQUIRE(rmt_transmit_fake.call_count == 0);      // nothing driven, no partial output
    REQUIRE(ArmPulseCapture_fake.call_count == 0);
    REQUIRE(HarnessGetTxChannel() == nullptr);
}

// ---- T-6: ApplyWs2812Timing() (FR-7, FR-17) ------------------------------------------------------------------------

TEST_CASE("ApplyWs2812Timing performs exactly one xQueueOverwrite and nothing else", "[T-6][FR-7]")
{
    StartController();
    xQueueOverwrite_fake.custom_fake = OverwriteSlot;
    g_slot_writes = 0;
    ApplyWs2812Timing(&kVectorX, 5);

    REQUIRE(fff.call_history_idx == 1);   // no RMT call, no wait, no ArmPulseCapture, no log
    REQUIRE(HistoryIndex(Fn(xQueueOverwrite)) == 0);
    REQUIRE(xQueueOverwrite_fake.arg0_val == HarnessGetTimingQueue());
    // Changed 2026-10-03: the queued item is a copy of (timing, submit_seq), not the caller's pointer.
    REQUIRE(SameTiming(g_slot.timing, kVectorX));
    REQUIRE(g_slot.submit_seq == 5);
}

TEST_CASE("two back-to-back ApplyWs2812Timing calls leave only the second value queued", "[T-6][FR-17]")
{
    StartController();
    xQueueOverwrite_fake.custom_fake = OverwriteSlot;
    g_slot_writes = 0;

    ApplyWs2812Timing(&kVectorC, 1);
    ApplyWs2812Timing(&kVectorX, 2);

    REQUIRE(g_slot_writes == 2);
    REQUIRE(SameTiming(g_slot.timing, kVectorX));
    REQUIRE(g_slot.submit_seq == 2);            // the pair is replaced together
    REQUIRE(xQueueSend_fake.call_count == 0);   // overwrite semantics, never a blocking send

    // The driver task then applies only the latest value, and arms with only the latest number (FR-33).
    HarnessRunDriverTaskRequests(&g_slot, 1);
    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(Values(g_last_bytes_update_config.bit1) == SymbolValues{48, 1, 32, 0});
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorX));
    REQUIRE(g_armed_submit_seq_count == 1);
    REQUIRE(g_armed_submit_seqs[0] == 2);
}

TEST_CASE("ApplyWs2812Timing is a no-op before start or with NULL", "[T-6][FR-7][FR-2]")
{
    ResetAll();
    ApplyWs2812Timing(&kVectorA, 1);   // queue not created yet
    REQUIRE(xQueueOverwrite_fake.call_count == 0);

    StartController();
    ApplyWs2812Timing(nullptr, 1);
    REQUIRE(xQueueOverwrite_fake.call_count == 0);
}

// ---- Driver task: FR-13, FR-16, FR-18, FR-24 ------------------------------------------------------------------------

TEST_CASE("driver task: one frame per queued timing, arm immediately before transmit", "[FR-13][FR-24]")
{
    StartController();
    REQUIRE(HarnessRunDriverTask(&kVectorX, 1) == 2);   // one frame, then blocks on the queue again

    REQUIRE(xQueueReceive_fake.arg0_history[0] == HarnessGetTimingQueue());
    REQUIRE(xQueueReceive_fake.arg2_history[0] == portMAX_DELAY);

    REQUIRE(rmt_bytes_encoder_update_config_fake.call_count == 1);
    REQUIRE(Values(g_last_bytes_update_config.bit0) == SymbolValues{40, 1, 40, 0});
    REQUIRE(Values(g_last_bytes_update_config.bit1) == SymbolValues{48, 1, 32, 0});
    REQUIRE(g_last_bytes_update_config.flags.msb_first == 1);
    REQUIRE(Values(*HarnessGetEncoderResetSymbol()) == SymbolValues{32000, 0, 0, 0});

    REQUIRE(ArmPulseCapture_fake.call_count == 1);
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorX));
    REQUIRE(g_armed_pixel_len_copy == LED_CONTROLLER_PIXEL_BYTES);
    REQUIRE(std::memcmp(g_armed_pixels_copy, kPattern, sizeof(kPattern)) == 0);
    REQUIRE(ArmPulseCapture_fake.arg2_val == HarnessGetPixelBuffer());   // 3rd argument since submit_seq was added

    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(rmt_transmit_fake.arg0_val == kFakeTxChannel);
    REQUIRE(rmt_transmit_fake.arg1_val == HarnessGetFrameEncoder());
    REQUIRE(rmt_transmit_fake.arg2_val == HarnessGetPixelBuffer());
    REQUIRE(rmt_transmit_fake.arg3_val == LED_CONTROLLER_PIXEL_BYTES);
    REQUIRE(std::memcmp(HarnessGetPixelBuffer(), kPattern, sizeof(kPattern)) == 0);

    // FR-24: ArmPulseCapture() is the call right before rmt_transmit().
    const int arm_at = HistoryIndex(Fn(ArmPulseCapture));
    REQUIRE(arm_at >= 0);
    REQUIRE(HistoryIndex(Fn(rmt_transmit)) == arm_at + 1);
    REQUIRE(HistoryIndex(Fn(rmt_bytes_encoder_update_config)) < arm_at);
}

TEST_CASE("driver task: completion is awaited with rmt_tx_wait_all_done and a 20 ms bound", "[FR-13][NFR-1][NFR-2]")
{
    StartController();
    HarnessRunDriverTask(&kVectorA, 1);

    REQUIRE(rmt_tx_wait_all_done_fake.call_count == 1);
    REQUIRE(rmt_tx_wait_all_done_fake.arg0_val == kFakeTxChannel);
    REQUIRE(rmt_tx_wait_all_done_fake.arg1_val == LED_CONTROLLER_FRAME_TIMEOUT_MS);
    REQUIRE(rmt_tx_wait_all_done_fake.arg1_val == 20);
    REQUIRE(HistoryIndex(Fn(rmt_transmit)) < HistoryIndex(Fn(rmt_tx_wait_all_done)));
}

TEST_CASE("driver task: a completed frame logs one Info line and the values at Debug", "[FR-18]")
{
    StartController();
    HarnessRunDriverTask(&kVectorC, 1);

    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);
    REQUIRE(LogContains("led_controller: strip driven"));
    REQUIRE(TestLogCount(LOG_LEVEL_DEBUG) == 1);
    REQUIRE(LogContains("bit0 high_ns=1075 period_ns=1200; bit1 high_ns=1100 period_ns=1200; reset_us=280"));
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
}

TEST_CASE("driver task: the boot semaphore is given after the first frame only", "[FR-1]")
{
    StartController();
    const ws2812_timing_t frames[] = {kVectorA, kVectorAC, kVectorX};
    HarnessRunDriverTask(frames, 3);

    REQUIRE(rmt_transmit_fake.call_count == 3);
    REQUIRE(xSemaphoreGive_fake.call_count == 1);
    REQUIRE(xSemaphoreGive_fake.arg0_val == HarnessGetBootDoneSemaphore());
    REQUIRE(HistoryIndex(Fn(rmt_tx_wait_all_done)) < HistoryIndex(Fn(xSemaphoreGive)));
}

TEST_CASE("T-7: rmt_transmit INVALID_STATE logs the transmit-busy Warning, skips the wait, and the task keeps running", "[T-7][FR-16]")
{
    StartController();
    esp_err_t results[] = {ESP_ERR_INVALID_STATE, ESP_OK};
    SET_RETURN_SEQ(rmt_transmit, results, 2);
    const ws2812_timing_t frames[] = {kVectorA, kVectorX};

    REQUIRE(HarnessRunDriverTask(frames, 2) == 3);   // both frames processed, back to waiting

    REQUIRE(rmt_transmit_fake.call_count == 2);
    REQUIRE(rmt_tx_wait_all_done_fake.call_count == 1);   // only for the successful frame
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
    REQUIRE(LogContains("led_controller: transmit busy, previous frame in flight (err=259)"));
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);            // only the second frame counts as driven
    REQUIRE(rmt_enable_fake.call_count == 0);              // no channel restart (FR-16)
    REQUIRE(rmt_disable_fake.call_count == 0);
}

TEST_CASE("T-7: a completion timeout logs a Warning; the next frame re-waits 20 ms and is skipped if still in flight", "[T-7][FR-16]")
{
    StartController();
    rmt_tx_wait_all_done_fake.return_val = ESP_ERR_TIMEOUT;
    const ws2812_timing_t frames[] = {kVectorA, kVectorX};
    REQUIRE(HarnessRunDriverTask(frames, 2) == 3);   // the task keeps running

    REQUIRE(rmt_transmit_fake.call_count == 1);          // the second frame is skipped
    REQUIRE(rmt_bytes_encoder_update_config_fake.call_count == 1);   // encoder not touched while in flight
    REQUIRE(ArmPulseCapture_fake.call_count == 1);
    REQUIRE(rmt_tx_wait_all_done_fake.call_count == 2);  // completion wait, then the one re-wait
    REQUIRE(rmt_tx_wait_all_done_fake.arg0_history[1] == kFakeTxChannel);
    REQUIRE(rmt_tx_wait_all_done_fake.arg1_history[1] == LED_CONTROLLER_FRAME_TIMEOUT_MS);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 2);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
    REQUIRE(LogContains("led_controller: frame completion timed out (err=263)"));
    REQUIRE(LogContains("led_controller: previous frame still in flight, frame skipped"));
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(HarnessIsFrameInFlight());                   // the next update checks again
    REQUIRE(rmt_enable_fake.call_count == 0);
    REQUIRE(rmt_disable_fake.call_count == 0);
}

TEST_CASE("T-7: after a timeout, a re-wait that completes lets the next frame through", "[T-7][FR-16]")
{
    StartController();
    // Frame 1: completion times out. Frame 2: re-wait still times out -> skipped.
    // Frame 3: re-wait completes -> update, arm, transmit, completion OK.
    esp_err_t waits[] = {ESP_ERR_TIMEOUT, ESP_ERR_TIMEOUT, ESP_OK, ESP_OK};
    SET_RETURN_SEQ(rmt_tx_wait_all_done, waits, 4);
    const ws2812_timing_t frames[] = {kVectorA, kVectorC, kVectorX};
    REQUIRE(HarnessRunDriverTask(frames, 3) == 4);

    REQUIRE(rmt_tx_wait_all_done_fake.call_count == 4);
    REQUIRE(rmt_transmit_fake.call_count == 2);          // frames 1 and 3
    REQUIRE(rmt_bytes_encoder_update_config_fake.call_count == 2);
    REQUIRE(Values(g_last_bytes_update_config.bit1) == SymbolValues{48, 1, 32, 0});   // frame 3 = vector X
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorX));
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 2);       // timed out + skipped
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);          // frame 3 driven
    REQUIRE_FALSE(HarnessIsFrameInFlight());
    // The re-wait of frame 3 (wait #3) happens before its encoder update.
    REQUIRE(HistoryIndex(Fn(rmt_tx_wait_all_done), 2) < HistoryIndex(Fn(rmt_bytes_encoder_update_config), 1));
}

TEST_CASE("T-7: without an earlier timeout there is no extra wait before the encoder update", "[T-7][FR-16]")
{
    StartController();
    const ws2812_timing_t frames[] = {kVectorA, kVectorX};
    HarnessRunDriverTask(frames, 2);
    REQUIRE(rmt_tx_wait_all_done_fake.call_count == 2);   // one completion wait per frame, nothing more
    REQUIRE(HistoryIndex(Fn(rmt_bytes_encoder_update_config), 1) > HistoryIndex(Fn(rmt_tx_wait_all_done), 0));
    REQUIRE(HistoryIndex(Fn(rmt_bytes_encoder_update_config), 1) < HistoryIndex(Fn(rmt_tx_wait_all_done), 1));
}

TEST_CASE("T-7: an encoder update failure logs an Error and neither arms nor transmits", "[T-7][FR-16][FR-24]")
{
    StartController();
    rmt_bytes_encoder_update_config_fake.return_val = ESP_ERR_INVALID_ARG;
    HarnessRunDriverTask(&kVectorA, 1);

    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(LogContains("failed to update bit encoding"));
    REQUIRE(ArmPulseCapture_fake.call_count == 0);
    REQUIRE(rmt_transmit_fake.call_count == 0);
}

TEST_CASE("FR-16 level mapping: Error for channel/encoder failures, Warning for transient ones", "[T-7][FR-16]")
{
    StartController();
    int expected_errors = 0;
    int expected_warnings = 0;
    const char *expected_text = "";
    SECTION("encoder update failure -> Error")
    {
        rmt_bytes_encoder_update_config_fake.return_val = ESP_FAIL;
        expected_errors = 1;
        expected_text = "led_controller: failed to update bit encoding";
    }
    SECTION("rmt_transmit ESP_ERR_INVALID_ARG -> Error")
    {
        rmt_transmit_fake.return_val = ESP_ERR_INVALID_ARG;
        expected_errors = 1;
        expected_text = "led_controller: transmit failed (err=258)";
    }
    SECTION("rmt_transmit ESP_FAIL -> Error")
    {
        rmt_transmit_fake.return_val = ESP_FAIL;
        expected_errors = 1;
        expected_text = "led_controller: transmit failed (err=-1)";
    }
    SECTION("rmt_transmit ESP_ERR_INVALID_STATE -> Warning")
    {
        rmt_transmit_fake.return_val = ESP_ERR_INVALID_STATE;
        expected_warnings = 1;
        expected_text = "led_controller: transmit busy, previous frame in flight (err=259)";
    }
    SECTION("completion-wait timeout -> Warning")
    {
        rmt_tx_wait_all_done_fake.return_val = ESP_ERR_TIMEOUT;
        expected_warnings = 1;
        expected_text = "led_controller: frame completion timed out (err=263)";
    }
    HarnessRunDriverTask(&kVectorA, 1);

    INFO(TestLogText());
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == expected_errors);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == expected_warnings);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(LogContains(expected_text));
    REQUIRE(rmt_enable_fake.call_count == 0);    // never a channel restart
    REQUIRE(rmt_disable_fake.call_count == 0);
}

TEST_CASE("FR-16 level mapping: a skipped frame (previous still in flight) is a Warning", "[T-7][FR-16]")
{
    StartController();
    rmt_tx_wait_all_done_fake.return_val = ESP_ERR_TIMEOUT;
    HarnessRunDriverTask(&kVectorA, 1);
    TestLogReset();
    HarnessRunDriverTask(&kVectorX, 1);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
    REQUIRE(LogContains("[L2 led_ctrl] led_controller: previous frame still in flight, frame skipped"));
}

TEST_CASE("rmt_transmit is called non-blocking (flags.queue_nonblocking = 1)", "[FR-16][FR-13][NFR-1]")
{
    StartController();
    rmt_transmit_fake.custom_fake = TransmitCopiesConfig;
    std::memset(&g_last_transmit_config, 0, sizeof(g_last_transmit_config));
    HarnessRunDriverTask(&kVectorA, 1);

    REQUIRE(rmt_transmit_fake.call_count == 1);
    REQUIRE(g_last_transmit_config.flags.queue_nonblocking == 1);
    REQUIRE(g_last_transmit_config.loop_count == 0);   // one frame, no loop
    REQUIRE(g_last_transmit_config.flags.eot_level == 0);
}

// ---- FR-8 / FR-3: static review of the source --------------------------------------------------------------------

TEST_CASE("static review: led_controller never bit-bangs GPIO8", "[FR-8][FR-3]")
{
    const std::string source = ReadSource(LED_CONTROLLER_SRC);
    REQUIRE(source.find("gpio_set_level") == std::string::npos);
    REQUIRE(source.find("gpio_config") == std::string::npos);
    for (const char *call : {"malloc(", "calloc(", "realloc(", " free("}) {
        INFO(call);
        REQUIRE(source.find(call) == std::string::npos);   // NFR-4 (comments mention "malloc/free")
    }
}

// ---- T-19 (FR-7, FR-16, FR-24, FR-33; 2026-10-03): submit_seq through the driver task -----------------------------------

TEST_CASE("T-19: the driver task passes each request's submit_seq to ArmPulseCapture, in order", "[T-19][FR-24][FR-33]")
{
    StartController();
    const led_request_t requests[] = {{kVectorA, 1}, {kVectorX, 2}, {kVectorAC, 3}, {kVectorAD, 4294967295u}};
    REQUIRE(HarnessRunDriverTaskRequests(requests, 4) == 5);

    REQUIRE(ArmPulseCapture_fake.call_count == 4);
    REQUIRE(g_armed_submit_seq_count == 4);
    REQUIRE(g_armed_submit_seqs[0] == 1);
    REQUIRE(g_armed_submit_seqs[1] == 2);
    REQUIRE(g_armed_submit_seqs[2] == 3);
    REQUIRE(g_armed_submit_seqs[3] == 4294967295u);   // unchanged, full 32 bits
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorAD));
    REQUIRE(rmt_transmit_fake.call_count == 4);
}

TEST_CASE("T-19: the boot frame is queued and armed with submit_seq 0", "[T-19][FR-1][FR-7][FR-33]")
{
    ResetAll();
    xQueueOverwrite_fake.custom_fake = OverwriteSlot;
    g_slot_writes = 0;
    REQUIRE(StartLedController());
    REQUIRE(g_slot_writes == 1);
    REQUIRE(g_slot.submit_seq == 0);

    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    PulseMonitorFakesReset();
    HarnessRunDriverTaskRequests(&g_slot, 1);   // the driver task picks the boot item up
    REQUIRE(ArmPulseCapture_fake.call_count == 1);
    REQUIRE(ArmPulseCapture_fake.arg1_val == 0u);
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorA));
}

TEST_CASE("T-19: two coalesced requests arm only the latest number", "[T-19][FR-17][FR-33]")
{
    StartController();
    xQueueOverwrite_fake.custom_fake = OverwriteSlot;
    ApplyWs2812Timing(&kVectorA, 7);
    ApplyWs2812Timing(&kVectorX, 8);
    ApplyWs2812Timing(&kVectorAD, 9);
    HarnessRunDriverTaskRequests(&g_slot, 1);   // only the 1-deep slot's content reaches the task

    REQUIRE(ArmPulseCapture_fake.call_count == 1);
    REQUIRE(g_armed_submit_seqs[0] == 9);       // 7 and 8 are never armed or published
    REQUIRE(SameTiming(g_armed_timing_copy, kVectorAD));
}

TEST_CASE("T-19: a frame skipped because the previous one is still in flight is never armed", "[T-19][FR-16][FR-33][FR-36]")
{
    StartController();
    rmt_tx_wait_all_done_fake.return_val = ESP_ERR_TIMEOUT;   // frame 1 times out, frame 2's re-wait too
    const led_request_t requests[] = {{kVectorA, 11}, {kVectorX, 12}};
    HarnessRunDriverTaskRequests(requests, 2);

    REQUIRE(LogContains("led_controller: previous frame still in flight, frame skipped"));
    REQUIRE(ArmPulseCapture_fake.call_count == 1);   // only seq 11; seq 12 publishes nothing (owner decision)
    REQUIRE(g_armed_submit_seqs[0] == 11);
    REQUIRE(rmt_transmit_fake.call_count == 1);
}

TEST_CASE("T-19: a frame whose encoder update fails is never armed", "[T-19][FR-16][FR-33][FR-36]")
{
    StartController();
    rmt_bytes_encoder_update_config_fake.return_val = ESP_ERR_INVALID_ARG;
    const led_request_t request = {kVectorX, 21};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(ArmPulseCapture_fake.call_count == 0);
    REQUIRE(rmt_transmit_fake.call_count == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
}

TEST_CASE("T-19: a transmit failure after a successful arm still armed that number (it ends in timeout, FR-36)", "[T-19][FR-16][FR-36]")
{
    StartController();
    rmt_transmit_fake.return_val = ESP_ERR_INVALID_STATE;
    const led_request_t request = {kVectorA, 31};
    HarnessRunDriverTaskRequests(&request, 1);
    REQUIRE(ArmPulseCapture_fake.call_count == 1);
    REQUIRE(g_armed_submit_seqs[0] == 31);
}

TEST_CASE("static review: led_controller publishes nothing itself (only rmt_pulse_monitor does)", "[T-19][FR-33][FR-36][NFR-17]")
{
    const std::string source = ReadSource(LED_CONTROLLER_SRC);
    for (const char *token : {"SetPulseResultCallback", "pulse_result_cb_t", "WS2812_MEASUREMENT_", "SetHttpTunerResult",
                              "http_portal.h", "provisioning.h"}) {
        INFO(token);
        REQUIRE(source.find(token) == std::string::npos);
    }
}
