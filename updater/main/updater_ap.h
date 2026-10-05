/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_ap.h
 * @brief Open SoftAP of the updater at 192.168.0.1 (SPEC-007 FR-22).
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief SoftAP channel. */
#define UPDATER_AP_CHANNEL (1)
/** @brief Maximum number of SoftAP clients. */
#define UPDATER_AP_MAX_CLIENTS (2)

/**
 * @brief Start the open SoftAP `RGB-LED-Updater-XXXX` (XXXX = last 4 hex digits of the SoftAP MAC)
 * on 192.168.0.1/24 with a DHCP server leasing from 192.168.0.2. Wi-Fi settings stay in RAM (FR-33).
 *
 * @return true if the AP is running.
 */
bool StartUpdaterAp(void);

#ifdef __cplusplus
}
#endif
