/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file button.c
 * @brief GPIO9 hold detection and sampling task.
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

button_event_t ProcessButtonSample(button_state_t *state, bool is_low, uint32_t now_ms, uint32_t hold_ms)
{
    if (!is_low) {
        state->is_pressed = false;
        state->is_hold_reported = false;
        return BUTTON_EVENT_NONE;
    }
    if (!state->is_pressed) {
        state->is_pressed = true;
        state->is_hold_reported = false;
        state->press_start_ms = now_ms;
        return BUTTON_EVENT_PRESS_STARTED;
    }
    if (!state->is_hold_reported && (now_ms - state->press_start_ms) >= hold_ms) {
        state->is_hold_reported = true;
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
        default:
            break;
        }
        vTaskDelayUntil(&last_wake_ticks, pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    }
}

bool StartButton(button_request_cb_t on_request)
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
    TaskHandle_t handle = xTaskCreateStatic(RunButtonTask, "button", BUTTON_TASK_STACK_BYTES, NULL,
                                            BUTTON_TASK_PRIORITY, s_task_stack, &s_task_struct);
    LOG_INFO("button service started on GPIO %d", BUTTON_GPIO_NUM);
    return handle != NULL;
}
