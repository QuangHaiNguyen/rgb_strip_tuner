/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file hello_world_main.c
 * @brief Application entry point: starts logging, the WS2812 pulse monitor and LED
 * controller, then hands over to Wi-Fi provisioning (SPEC-004 FR-1, FR-19).
 */
#include "led_controller.h"
#include "logging.h"
#include "provisioning.h"
#include "rmt_pulse_monitor.h"

LOG_MODULE_REGISTER("main", LOG_LEVEL_DEBUG);

/** @brief ESP-IDF entry point. */
void app_main(void)
{
    LogInit();
    LOG_INFO("logging initialized");

    /* StartPulseMonitor() first (SPEC-004 FR-19), so the RX channel is armable
     * before StartLedController()'s boot-time default frame is transmitted. */
    if (!StartPulseMonitor()) {
        LOG_ERROR("pulse monitor failed to start; WS2812 pulses will not be measured");
    }
    if (!StartLedController()) {
        LOG_ERROR("led controller failed to start; strip will not be driven");
    }

    ProvisioningStart();
    LOG_INFO("startup sequence handed off to provisioning");
}

