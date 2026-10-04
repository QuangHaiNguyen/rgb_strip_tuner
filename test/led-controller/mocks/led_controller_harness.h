#pragma once
/**
 * @file led_controller_harness.h
 * @brief Test access to led_controller.c's file-scope state and static functions.
 *
 * led_controller_harness.c compiles led_controller.c into the test (same trick as
 * test/<suite>/mocks/http_portal_harness.c), so the static symbol builders (FR-10/FR-11), the
 * encoder callbacks (FR-13) and the driver task body are reachable without an RMT peripheral.
 */
#include <stdbool.h>
#include <stdint.h>
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "led_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Forget all file-scope state (channel, encoder, queue, semaphore, pixel buffer, in-flight flag). */
void HarnessResetLedController(void);

/** Static BuildBytesEncoderConfig() / BuildResetSymbol() (FR-10, FR-11). */
rmt_bytes_encoder_config_t HarnessBuildBytesEncoderConfig(const ws2812_timing_t *timing);
rmt_symbol_word_t HarnessBuildResetSymbol(const ws2812_timing_t *timing);

/** The custom GRB + reset encoder (FR-13) and its current reset symbol. */
rmt_encoder_t *HarnessGetFrameEncoder(void);
const rmt_symbol_word_t *HarnessGetEncoderResetSymbol(void);
/** Initialize the frame encoder as StartLedController() does (static InitWs2812Encoder()). */
bool HarnessInitEncoder(const ws2812_timing_t *timing);

QueueHandle_t HarnessGetTimingQueue(void);
SemaphoreHandle_t HarnessGetBootDoneSemaphore(void);
rmt_channel_handle_t HarnessGetTxChannel(void);
const uint8_t *HarnessGetPixelBuffer(void);
/** The task entry point StartLedController() handed to xTaskCreateStatic(). */
TaskFunction_t HarnessGetDriverTaskFunction(void);
/** True while a frame whose completion wait timed out may still be in flight (FR-16). */
bool HarnessIsFrameInFlight(void);

/**
 * Run the real driver task loop (static RunLedDriverTask()) until its queue runs dry.
 *
 * Installs an xQueueReceive() fake that delivers @p timings one per iteration for the timing
 * queue (with portMAX_DELAY), then leaves the endless loop with longjmp() when none is left.
 * @return Number of xQueueReceive() calls the task made (count + 1 when it ran dry).
 */
int HarnessRunDriverTask(const ws2812_timing_t *timings, int count);

/** As HarnessRunDriverTask(), but delivers whole led_request_t items (timing + submit_seq, SPEC-004 FR-7/FR-33).
 *  HarnessRunDriverTask() delivers each timing with submit_seq 0. */
int HarnessRunDriverTaskRequests(const led_request_t *requests, int count);

#ifdef __cplusplus
}
#endif
