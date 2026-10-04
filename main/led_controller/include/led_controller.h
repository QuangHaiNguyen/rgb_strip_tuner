/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file led_controller.h
 * @brief WS2812 RMT TX driver for a fixed 6-LED strip with tunable timing (SPEC-004).
 *
 * Owns GPIO8 and one ESP32-C3 RMT TX channel. The strip is driven with a
 * fixed, compile-time R/R/G/G/B/B color pattern (section 7.1) using a
 * tunable bit-0/bit-1/reset timing set (ws2812_timing_t, SPEC-003). At boot
 * the strip is driven once with the SPEC-003 default timing; thereafter
 * ApplyWs2812Timing() re-drives it with every valid `POST /tuner` submission,
 * routed through the `provisioning` orchestrator queue (SPEC-004 FR-4..FR-7).
 *
 * Ws2812NsToTicks() and BuildLedPixelPattern() are pure functions with no
 * ESP-IDF/RMT dependency (NFR-10); all RMT/GPIO hardware interaction is
 * exercised only through StartLedController()/ApplyWs2812Timing().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ws2812_timing.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed LED count for this phase (section 7.1). */
#define LED_CONTROLLER_LED_COUNT (6)
/** @brief GRB pixel buffer size in bytes (LED_CONTROLLER_LED_COUNT * 3). */
#define LED_CONTROLLER_PIXEL_BYTES (LED_CONTROLLER_LED_COUNT * 3)
/** @brief The one active color channel value per LED, out of 255 (FR-15). */
#define LED_CONTROLLER_CHANNEL_VALUE (32)
/** @brief WS2812 data-line GPIO; sole owner (FR-3). */
#define LED_CONTROLLER_GPIO_NUM (8)
/** @brief RMT TX channel resolution, in Hz (25 ns/tick). */
#define LED_CONTROLLER_RESOLUTION_HZ (40000000)
/** @brief RMT TX channel tick duration, in ns (1e9 / LED_CONTROLLER_RESOLUTION_HZ). */
#define LED_CONTROLLER_TICK_NS (25)
/** @brief Completion-wait timeout for one frame, in ms (NFR-2: >= 1.2 ms, <= 20 ms, >= 2 ticks; 2 ticks at 100 Hz). */
#define LED_CONTROLLER_FRAME_TIMEOUT_MS (20)

/**
 * @brief One timing update for the driver task (FR-7), 16 bytes: the timing set and the
 * SPEC-003 FR-23 submission sequence number of the request (0 for the boot frame).
 */
typedef struct {
    ws2812_timing_t timing; /**< Validated timing set to apply. */
    uint32_t submit_seq;    /**< Submission sequence number; 0 = boot frame. */
} led_request_t;

/**
 * @brief Convert a duration in nanoseconds to an RMT tick count (FR-9).
 *
 * Pure function, round-half-up: ticks = (duration_ns + tick_ns / 2) / tick_ns.
 * No ESP-IDF/RMT dependency (NFR-10).
 *
 * @param[in] duration_ns Duration to convert, in nanoseconds.
 * @param[in] tick_ns     Duration of one tick, in nanoseconds.
 * @return Tick count.
 */
uint16_t Ws2812NsToTicks(uint32_t duration_ns, uint32_t tick_ns);

/**
 * @brief Fill the fixed 6-LED R/R/G/G/B/B GRB pixel pattern (FR-12).
 *
 * Pure function, independent of any timing set. LED 0 is nearest GPIO8.
 * No ESP-IDF/RMT dependency (NFR-10).
 *
 * @param[out] pixel_grb Receives LED_CONTROLLER_PIXEL_BYTES bytes, GRB, MSB-first wire order.
 */
void BuildLedPixelPattern(uint8_t pixel_grb[LED_CONTROLLER_PIXEL_BYTES]);

/**
 * @brief Configure GPIO8, create the RMT TX channel, start the driver task, and drive one
 * boot-time frame with the SPEC-003 default timing (FR-1).
 *
 * Call once from app_main(), before ProvisioningStart(). On failure, logs at Error level
 * and returns false without preventing the rest of app_main() from running (FR-2).
 *
 * @return true if GPIO8/RMT channel/encoder/task were created successfully.
 */
bool StartLedController(void);

/**
 * @brief Queue a new timing set for the driver task to apply (FR-7).
 *
 * Non-blocking: only overwrites the driver task's 1-deep queue (xQueueOverwrite())
 * with the (timing, submit_seq) pair and returns; never touches GPIO8/RMT directly and
 * never blocks on an in-progress transmission. Safe to call from any task (e.g. the
 * provisioning orchestrator). The driver task hands @p submit_seq on to
 * ArmPulseCapture() (FR-33).
 *
 * @param[in] timing     Timing set to apply on the next frame.
 * @param[in] submit_seq Submission sequence number of the request (SPEC-003 FR-23); 0 for the boot frame.
 */
void ApplyWs2812Timing(const ws2812_timing_t *timing, uint32_t submit_seq);

#ifdef __cplusplus
}
#endif
