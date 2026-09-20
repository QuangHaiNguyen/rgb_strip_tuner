/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file button.h
 * @brief GPIO9 provisioning-button service (SPEC-002 FR-1, FR-2, FR-24).
 *
 * The button is active low. A continuous press of BUTTON_HOLD_MS requests
 * provisioning; the request is issued at the first 10 ms sample at or after
 * the threshold, i.e. within BUTTON_HOLD_MS + BUTTON_SAMPLE_MS of press start.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Provisioning button GPIO number. */
#define BUTTON_GPIO_NUM (9)
/** @brief GPIO sampling period in milliseconds. */
#define BUTTON_SAMPLE_MS (10)
/** @brief Continuous press duration that requests provisioning, in milliseconds. */
#define BUTTON_HOLD_MS (1000)

/** @brief Result of evaluating one GPIO sample. */
typedef enum {
    BUTTON_EVENT_NONE = 0,       /**< Nothing to report. */
    BUTTON_EVENT_PRESS_STARTED,  /**< First low sample after a high sample. */
    BUTTON_EVENT_HOLD_REACHED,   /**< The hold threshold was reached during this press. */
} button_event_t;

/** @brief Hold-timer state for ProcessButtonSample(). Zero-initialize before first use. */
typedef struct {
    bool is_pressed;         /**< The previous sample was low. */
    bool is_hold_reported;   /**< HOLD_REACHED already returned for the current press. */
    uint32_t press_start_ms; /**< Timestamp of the press start. */
} button_state_t;

/** @brief Called from the button task when a provisioning request is detected. */
typedef void (*button_request_cb_t)(void);

/**
 * @brief Evaluate one GPIO sample (pure function, no hardware access).
 *
 * A release resets the hold timer. HOLD_REACHED is returned once per press.
 *
 * @param[in,out] state   Hold-timer state.
 * @param[in]     is_low  true if the GPIO reads low (pressed).
 * @param[in]     now_ms  Monotonic time in milliseconds.
 * @param[in]     hold_ms Hold threshold in milliseconds.
 * @return Event produced by this sample.
 */
button_event_t ProcessButtonSample(button_state_t *state, bool is_low, uint32_t now_ms, uint32_t hold_ms);

/**
 * @brief Configure GPIO9 as a pulled-up input and start the sampling task.
 *
 * The task runs until shutdown, in every operating state.
 *
 * @param on_request Callback invoked from the button task on each hold request.
 * @return true if the task was started.
 */
bool StartButton(button_request_cb_t on_request);

#ifdef __cplusplus
}
#endif
