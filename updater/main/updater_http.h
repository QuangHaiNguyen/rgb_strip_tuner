/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_http.h
 * @brief HTTP server of updater mode: page, streaming upload and status (SPEC-007 FR-23 to FR-30).
 */
#pragma once

#include <stdbool.h>
#include "esp_partition.h"
#include "updater_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Largest receive chunk of an upload, in bytes; one static buffer (FR-27, NFR-3). */
#define FW_UPLOAD_CHUNK_BYTES (4096)
/** @brief Receive timeouts tolerated in a row during an upload (FR-27). */
#define FW_UPLOAD_RECV_RETRIES (5)
/** @brief Delay between a successful upload's response and the restart, in milliseconds (FR-28). */
#define FW_RESTART_DELAY_MS (1000)
/** @brief Upload progress is logged at Debug level at most every this many bytes (FR-30). */
#define FW_UPLOAD_LOG_STEP_BYTES (64u * 1024u)
/** @brief HTTP server task stack, in bytes (NFR-3). It serves the page, /status and the 409 only. */
#define UPDATER_HTTP_STACK_BYTES (4096)
/** @brief Upload worker task stack, in bytes (FR-42): esp_ota_*, flash, SHA-256 and the response. */
#define UPDATER_UPLOAD_STACK_BYTES (6144)
/** @brief Open sockets: the upload plus two other connections (FR-42, at most 2 clients). */
#define UPDATER_HTTP_MAX_SOCKETS (3)

/**
 * @brief Start the HTTP server on port 80 with GET /, POST /update and GET /status (FR-23).
 *
 * POST /update is handed to a static upload worker task with httpd_req_async_handler_begin()
 * (FR-42), so GET / and GET /status are served during an upload and a second POST /update gets
 * an immediate 409. The upload state is shared between the two tasks under one mutex.
 *
 * @param reason            Updater-mode reason shown on the page (FR-26).
 * @param ota               The ota_0 partition the upload is written to.
 * @param installed_version Version of the installed firmware, or NULL for none.
 * @return true if the server is running.
 */
bool StartUpdaterHttp(updater_reason_t reason, const esp_partition_t *ota, const char *installed_version);

#ifdef __cplusplus
}
#endif
