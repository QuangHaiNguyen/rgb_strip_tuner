/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file hello_world_main.c
 * @brief Application entry point: starts logging and hands over to Wi-Fi provisioning.
 */
#include "logging.h"
#include "provisioning.h"

LOG_MODULE_REGISTER("main", LOG_LEVEL_DEBUG);

/** @brief ESP-IDF entry point. */
void app_main(void)
{
    LogInit();
    LOG_INFO("logging initialized");
    ProvisioningStart();
    LOG_INFO("startup sequence handed off to provisioning");
}

