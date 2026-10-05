/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file button.h
 * @brief GPIO9 provisioning-button service (SPEC-002 FR-1, FR-2, FR-24) and update
 * gesture (SPEC-007 FR-9).
 *
 * The button is active low. A continuous press of BUTTON_HOLD_MS requests
 * provisioning; the request is issued at the first 10 ms sample at or after
 * the threshold, i.e. within BUTTON_HOLD_MS + BUTTON_SAMPLE_MS of press start.
 *
 * FW_UPDATE_GESTURE_PRESSES complete presses, each shorter than BUTTON_HOLD_MS,
 * whose press starts lie within FW_UPDATE_GESTURE_WINDOW_MS, request the updater;
 * the request is issued at the first sample after the last release. The window
 * slides over the last FW_UPDATE_GESTURE_PRESSES press starts, so an earlier stray
 * press does not prevent a following valid sequence. A press that reaches
 * BUTTON_HOLD_MS, or a release followed by no press start within
 * FW_UPDATE_GESTURE_WINDOW_MS, resets the gesture count; so does a recognized gesture.
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
/** @brief Number of short presses of the update gesture (SPEC-007 FR-9). */
#define FW_UPDATE_GESTURE_PRESSES (5)
/** @brief Window in which the press starts of the update gesture lie, in milliseconds (SPEC-007 FR-9). */
#define FW_UPDATE_GESTURE_WINDOW_MS (3000)

/** @brief Result of evaluating one GPIO sample. */
typedef enum {
    BUTTON_EVENT_NONE = 0,       /**< Nothing to report. */
    BUTTON_EVENT_PRESS_STARTED,  /**< First low sample after a high sample. */
    BUTTON_EVENT_HOLD_REACHED,   /**< The hold threshold was reached during this press. */
    BUTTON_EVENT_UPDATE_GESTURE, /**< Release completing the update gesture (SPEC-007 FR-9). */
} button_event_t;

/** @brief Hold-timer and gesture state for ProcessButtonSample(). Zero-initialize before first use. */
typedef struct {
    bool is_pressed;            /**< The previous sample was low. */
    bool is_hold_reported;      /**< HOLD_REACHED already returned for the current press. */
    uint32_t press_start_ms;    /**< Timestamp of the press start. */
    uint32_t gesture_starts_ms[FW_UPDATE_GESTURE_PRESSES]; /**< Ring of the last short-press start times. */
    uint8_t gesture_next;       /**< Ring index of the next press start (the oldest one when the ring is full). */
    uint8_t gesture_presses;    /**< Press starts in the ring, at most FW_UPDATE_GESTURE_PRESSES. */
    uint32_t release_ms;        /**< Timestamp of the last release. */
} button_state_t;

/** @brief Called from the button task when a provisioning or update request is detected. */
typedef void (*button_request_cb_t)(void);

/**
 * @brief Evaluate one GPIO sample (pure function, no hardware access).
 *
 * A release resets the hold timer. HOLD_REACHED is returned once per press.
 * UPDATE_GESTURE is returned on the release that completes the update gesture.
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
 * @param on_update  Callback invoked from the button task on each update gesture (SPEC-007 FR-9), or NULL.
 * @return true if the task was started.
 */
bool StartButton(button_request_cb_t on_request, button_request_cb_t on_update);

#ifdef __cplusplus
}
#endif
