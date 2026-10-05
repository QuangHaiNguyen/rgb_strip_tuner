/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file hello_world_main.c
 * @brief Application entry point: starts logging, the WS2812 pulse monitor and LED
 * controller, then hands over to Wi-Fi provisioning (SPEC-004 FR-1, FR-19).
 *
 * The firmware runs from ota_0 and is started by the updater (SPEC-007 FR-32, FR-40).
 */
#include "fw_build_config.h"
#include "fw_update.h"
#include "led_controller.h"
#include "logging.h"
#include "provisioning.h"
#include "rmt_pulse_monitor.h"

LOG_MODULE_REGISTER("main", LOG_LEVEL_DEBUG);

/** @brief ESP-IDF entry point. */
void app_main(void)
{
    LogInit();
    /* SPEC-007 FR-37: first log line, naming the version and the build configuration. */
    LOG_INFO("firmware %s, build configuration: %s", GetFwVersion(), FW_BUILD_CONFIG_NAME);
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

