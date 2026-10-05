/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file button.c
 * @brief GPIO9 hold detection, update-gesture detection (SPEC-007 FR-9) and sampling task.
 */
#include "button.h"
#include "logging.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_TASK_STACK_BYTES (2048)
#define BUTTON_TASK_PRIORITY (5)

LOG_MODULE_REGISTER("button", LOG_LEVEL_DEBUG);

static StaticTask_t s_task_struct;
static StackType_t s_task_stack[BUTTON_TASK_STACK_BYTES];
static button_request_cb_t s_on_request;
static button_request_cb_t s_on_update;

/** @brief SPEC-007 FR-9: forget the counted press starts. */
static void ResetGesture(button_state_t *state)
{
    state->gesture_presses = 0;
    state->gesture_next = 0;
}

/**
 * @brief SPEC-007 FR-9: true if the ring holds FW_UPDATE_GESTURE_PRESSES short-press starts and the
 * newest lies at most FW_UPDATE_GESTURE_WINDOW_MS after the oldest (sliding window).
 */
static bool IsGestureComplete(const button_state_t *state)
{
    if (state->gesture_presses < FW_UPDATE_GESTURE_PRESSES) {
        return false;
    }
    uint32_t oldest_ms = state->gesture_starts_ms[state->gesture_next];
    uint32_t newest_ms = state->gesture_starts_ms[(state->gesture_next + FW_UPDATE_GESTURE_PRESSES - 1) %
                                                  FW_UPDATE_GESTURE_PRESSES];
    return (newest_ms - oldest_ms) <= FW_UPDATE_GESTURE_WINDOW_MS;
}

/**
 * @brief SPEC-007 FR-9: evaluate a high sample for the update gesture.
 *
 * On a release edge, completes the gesture if the last press starts form one (a press that
 * reached the hold threshold has already reset the count). While released, drops the count once
 * no press has started within FW_UPDATE_GESTURE_WINDOW_MS of the last release.
 */
static button_event_t ProcessGestureRelease(button_state_t *state, uint32_t now_ms)
{
    if (state->is_pressed) {
        state->release_ms = now_ms;
        if (!state->is_hold_reported && IsGestureComplete(state)) {
            ResetGesture(state);
            return BUTTON_EVENT_UPDATE_GESTURE;
        }
    } else if (state->gesture_presses > 0 && (now_ms - state->release_ms) >= FW_UPDATE_GESTURE_WINDOW_MS) {
        ResetGesture(state);
    }
    return BUTTON_EVENT_NONE;
}

/** @brief SPEC-007 FR-9: record a press start in the ring, replacing the oldest when it is full. */
static void CountGesturePress(button_state_t *state, uint32_t now_ms)
{
    state->gesture_starts_ms[state->gesture_next] = now_ms;
    state->gesture_next = (uint8_t)((state->gesture_next + 1) % FW_UPDATE_GESTURE_PRESSES);
    if (state->gesture_presses < FW_UPDATE_GESTURE_PRESSES) {
        ++state->gesture_presses;
    }
}

button_event_t ProcessButtonSample(button_state_t *state, bool is_low, uint32_t now_ms, uint32_t hold_ms)
{
    if (!is_low) {
        button_event_t event = ProcessGestureRelease(state, now_ms);
        state->is_pressed = false;
        state->is_hold_reported = false;
        return event;
    }
    if (!state->is_pressed) {
        state->is_pressed = true;
        state->is_hold_reported = false;
        state->press_start_ms = now_ms;
        CountGesturePress(state, now_ms);
        return BUTTON_EVENT_PRESS_STARTED;
    }
    if (!state->is_hold_reported && (now_ms - state->press_start_ms) >= hold_ms) {
        state->is_hold_reported = true;
        ResetGesture(state); /* a provisioning hold is never part of the update gesture */
        return BUTTON_EVENT_HOLD_REACHED;
    }
    return BUTTON_EVENT_NONE;
}

static void RunButtonTask(void *arg)
{
    (void)arg;
    button_state_t state = {0};
    TickType_t last_wake_ticks = xTaskGetTickCount();

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        switch (ProcessButtonSample(&state, gpio_get_level(BUTTON_GPIO_NUM) == 0, now_ms, BUTTON_HOLD_MS)) {
        case BUTTON_EVENT_PRESS_STARTED:
            LOG_INFO("button press started");
            break;
        case BUTTON_EVENT_HOLD_REACHED:
            LOG_INFO("button held %d ms, provisioning requested", BUTTON_HOLD_MS);
            if (s_on_request != NULL) {
                s_on_request();
            }
            break;
        case BUTTON_EVENT_UPDATE_GESTURE:
            LOG_INFO("button pressed %d times, update requested", FW_UPDATE_GESTURE_PRESSES);
            if (s_on_update != NULL) {
                s_on_update();
            }
            break;
        default:
            break;
        }
        vTaskDelayUntil(&last_wake_ticks, pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

bool StartButton(button_request_cb_t on_request, button_request_cb_t on_update)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO_NUM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        LOG_ERROR("failed to configure GPIO %d", BUTTON_GPIO_NUM);
        return false;
    }

    s_on_request = on_request;
    s_on_update = on_update;
    TaskHandle_t handle = xTaskCreateStatic(RunButtonTask, "button", BUTTON_TASK_STACK_BYTES, NULL,
                                            BUTTON_TASK_PRIORITY, s_task_stack, &s_task_struct);
    LOG_INFO("button service started on GPIO %d", BUTTON_GPIO_NUM);
    return handle != NULL;
}
