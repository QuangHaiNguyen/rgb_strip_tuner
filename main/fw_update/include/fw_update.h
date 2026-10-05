/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_update.h
 * @brief Firmware side of the firmware updater (SPEC-007 FR-10, FR-11, FR-13).
 *
 * Owns the embedded metadata of this image (FR-13), sets the force-bootloader flag on an update
 * request (FR-10), and clears the crash count after a healthy run (FR-11). The orchestrator
 * (provisioning) calls these functions from its own task.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Uptime after which the firmware counts as healthy and clears the crash count, in milliseconds.
 * The orchestrator (provisioning) times it and then calls MarkFirmwareHealthy(). */
#define FW_HEALTHY_UPTIME_MS (30000)
/** @brief Time given to the log writer before the restart into the updater, in milliseconds. */
#define FW_LOG_DRAIN_MS (100)

/** @brief Result of RequestFwUpdate(). */
typedef enum {
    FW_UPDATE_REQUEST_SET = 0,   /**< Force-bootloader flag programmed and read back: restart. */
    FW_UPDATE_REQUEST_NO_RECORD, /**< The record is invalid, nothing to set (the updater stays anyway): restart. */
    FW_UPDATE_REQUEST_FAILED,    /**< Write or read-back failed (Error logged): keep running. */
} fw_update_request_t;

/**
 * @brief Version of this firmware image, from its embedded metadata (FR-13).
 *
 * @return "MM.mm.pp".
 */
const char *GetFwVersion(void);

/**
 * @brief Handle an update request (FR-10): log it and set the force-bootloader flag of the
 * metadata record with a 1-to-0 program (no erase), then read it back.
 *
 * @return What the caller does next (restart or keep running).
 */
fw_update_request_t RequestFwUpdate(void);

/**
 * @brief Wait FW_LOG_DRAIN_MS for the log to drain, then restart into the updater (FR-10). Does not return.
 *
 * The caller stops the HTTP server and Wi-Fi first.
 */
void RestartIntoUpdater(void);

/**
 * @brief Clear the crash count after a healthy run (FR-11): one control-record write, only if the
 * count is non-zero.
 */
void MarkFirmwareHealthy(void);

/**
 * @brief Fallback for FR-11 when the orchestrator cannot start (Wi-Fi manager init failed): a
 * one-shot esp_timer that calls MarkFirmwareHealthy() FW_HEALTHY_UPTIME_MS after this call, from
 * the esp_timer task.
 *
 * @return true if the timer was started.
 */
bool StartFwHealthyTimer(void);

#ifdef __cplusplus
}
#endif
