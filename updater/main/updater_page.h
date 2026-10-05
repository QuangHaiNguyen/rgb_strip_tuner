/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_page.h
 * @brief The updater's upload page and its reason texts (SPEC-007 FR-24 to FR-26).
 *
 * The page is served as head + installed version + middle + reason text + tail, all from flash.
 * Its largest possible size is UPDATER_PAGE_MAX_BYTES or less (checked at compile time).
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Size cap of the page in bytes (FR-24). */
#define UPDATER_PAGE_MAX_BYTES (3072)

/** @brief Why the updater is in updater mode (FR-7). */
typedef enum {
    UPDATER_REASON_REQUESTED = 0,      /**< Force-bootloader flag (5-press gesture). */
    UPDATER_REASON_NO_FIRMWARE,        /**< No valid record, or the record does not match ota_0. */
    UPDATER_REASON_CRASH_LOOP,         /**< FW_CRASH_RESET_LIMIT consecutive crash resets. */
    UPDATER_REASON_BOOT_SELECT_FAILED, /**< The FR-40 jump could not be prepared or did not land. */
} updater_reason_t;

/** @brief Page text up to "Installed firmware: ". */
extern const char g_updater_page_head[];
/** @brief Page text between the installed version and the reason line. */
extern const char g_updater_page_middle[];
/** @brief Page text after the reason line: file input, Upload button, status region and script. */
extern const char g_updater_page_tail[];

/**
 * @brief Log token of a reason, as in `entering updater mode (reason=<token>)` (FR-7).
 *
 * @param reason Reason.
 * @return "requested", "no_firmware", "crash_loop" or "boot_select_failed".
 */
const char *GetUpdaterReasonName(updater_reason_t reason);

/**
 * @brief Reason line of the page (FR-26), from a fixed table.
 *
 * @param reason Reason.
 * @return Reason text.
 */
const char *GetUpdaterReasonText(updater_reason_t reason);

/**
 * @brief Largest size the assembled page can have, in bytes (for T-7).
 *
 * @return Size of head + middle + tail + the longest version and reason text.
 */
size_t GetUpdaterPageMaxBytes(void);

#ifdef __cplusplus
}
#endif
