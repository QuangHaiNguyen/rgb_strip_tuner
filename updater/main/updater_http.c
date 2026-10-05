/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_http.c
 * @brief Updater-mode HTTP server: GET / (page), POST /update (streaming upload, validation and
 * commit) and GET /status (SPEC-007 FR-23 to FR-30, FR-42).
 *
 * Upload order (FR-27, FR-20, owner answer 4): the metadata record is erased before the first
 * write to ota_0 and written only after esp_ota_end() has verified the image, so an interrupted
 * or rejected upload always leaves the device in updater mode at the next start (NFR-5).
 *
 * Tasks (FR-42): the HTTP server task claims the upload (state idle/error -> uploading) under
 * s_state_mutex, hands the request to the upload worker task through a one-item queue with
 * httpd_req_async_handler_begin(), and returns. The worker streams the image and replies, then
 * ends the request with httpd_req_async_handler_complete(). A POST /update that finds the state
 * uploading or done gets an immediate 409 from the server task: its body is not read and flash is
 * not touched. The upload state, the byte counters and the installed version are only accessed
 * under s_state_mutex; the chunk and head buffers are used by the worker alone.
 */
#include "updater_http.h"
#include <stdio.h>
#include <string.h>
#include "fw_meta.h"
#include "fw_meta_flash.h"
#include "logging.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#define STATUS_BODY_MAX_LEN (96)
#define UPLOAD_WORKER_PRIORITY (5)

LOG_MODULE_REGISTER("updater", LOG_LEVEL_DEBUG);

/** @brief Upload state reported by GET /status (FR-29). */
typedef enum {
    UPLOAD_STATE_IDLE = 0,
    UPLOAD_STATE_UPLOADING,
    UPLOAD_STATE_DONE,
    UPLOAD_STATE_ERROR,
} upload_state_t;

/** @brief Upload outcome; indexes s_upload_replies. */
typedef enum {
    UPLOAD_OK = 0,
    UPLOAD_REJECT_SIZE,
    UPLOAD_REJECT_IMAGE,
    UPLOAD_REJECT_METADATA,
    UPLOAD_REJECT_VERSION,
    UPLOAD_REJECT_VERIFY,
    UPLOAD_REJECT_FLASH,
    UPLOAD_REJECT_INTERRUPTED,
} upload_result_t;

/** @brief Fixed response and log token of one upload outcome (FR-28). */
typedef struct {
    const char *status; /**< HTTP status line. */
    const char *body;   /**< text/plain body. */
    const char *token;  /**< Warning token `upload rejected (reason=<token>)`. */
} upload_reply_t;

static const upload_reply_t s_upload_replies[] = {
    [UPLOAD_OK] = {"200 OK", "Update complete, restarting", ""},
    [UPLOAD_REJECT_SIZE] = {"400 Bad Request", "Invalid size", "size"},
    [UPLOAD_REJECT_IMAGE] = {"400 Bad Request", "Not an ESP32-C3 firmware image", "image"},
    [UPLOAD_REJECT_METADATA] = {"400 Bad Request", "Missing firmware metadata", "metadata"},
    [UPLOAD_REJECT_VERSION] = {"400 Bad Request", "Invalid firmware version", "version"},
    [UPLOAD_REJECT_VERIFY] = {"400 Bad Request", "Image verification failed", "verify"},
    [UPLOAD_REJECT_FLASH] = {"500 Internal Server Error", "Flash write failed", "flash"},
    [UPLOAD_REJECT_INTERRUPTED] = {"408 Request Timeout", "Upload interrupted", "interrupted"},
};

static const char *const s_upload_state_names[] = {
    [UPLOAD_STATE_IDLE] = "idle",
    [UPLOAD_STATE_UPLOADING] = "uploading",
    [UPLOAD_STATE_DONE] = "done",
    [UPLOAD_STATE_ERROR] = "error",
};

static httpd_handle_t s_server;
static const esp_partition_t *s_ota;
static updater_reason_t s_reason;

/* Shared between the HTTP server task and the upload worker: under s_state_mutex. */
static char s_installed_version[FW_VERSION_FIELD_LEN]; /* empty = none */
static upload_state_t s_upload_state;
static size_t s_received_bytes;
static size_t s_total_bytes;
static StaticSemaphore_t s_state_mutex_struct;
static SemaphoreHandle_t s_state_mutex;

/* Upload worker only. */
static uint8_t s_chunk[FW_UPLOAD_CHUNK_BYTES];
static uint8_t s_head[FW_IMAGE_HEAD_LEN];
static StaticTimer_t s_restart_timer_struct;

/* Upload worker task and its one-item request queue (FR-42). */
static StaticTask_t s_worker_struct;
static StackType_t s_worker_stack[UPDATER_UPLOAD_STACK_BYTES];
static StaticQueue_t s_upload_queue_struct;
static uint8_t s_upload_queue_storage[sizeof(httpd_req_t *)];
static QueueHandle_t s_upload_queue;

/** @brief Take the upload-state mutex. */
static void LockState(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
}

/** @brief Give the upload-state mutex. */
static void UnlockState(void)
{
    xSemaphoreGive(s_state_mutex);
}

/** @brief Set the upload state (FR-29). */
static void SetUploadState(upload_state_t state)
{
    LockState();
    s_upload_state = state;
    UnlockState();
}

/** @brief Set the installed version shown on the page and in /status; NULL for none. */
static void SetInstalledVersion(const char *version)
{
    LockState();
    memset(s_installed_version, 0, sizeof(s_installed_version));
    if (version != NULL) {
        memcpy(s_installed_version, version, sizeof(s_installed_version) - 1);
    }
    UnlockState();
}

/** @brief Copy the installed version for the page and /status, "none" if there is none. */
static void CopyInstalledVersion(char *version, size_t version_size)
{
    LockState();
    snprintf(version, version_size, "%s", (s_installed_version[0] != '\0') ? s_installed_version : "none");
    UnlockState();
}

/** @brief Send a fixed text/plain response with Cache-Control: no-store. */
static esp_err_t SendText(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/** @brief FR-24: GET / serves the page from flash, with the installed version and the reason line. */
static esp_err_t HandlePage(httpd_req_t *req)
{
    char version[FW_VERSION_FIELD_LEN];
    CopyInstalledVersion(version, sizeof(version));
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const char *const parts[] = {g_updater_page_head, version, g_updater_page_middle, GetUpdaterReasonText(s_reason),
                                 g_updater_page_tail};
    for (size_t index = 0; index < sizeof(parts) / sizeof(parts[0]); ++index) {
        if (httpd_resp_sendstr_chunk(req, parts[index]) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/** @brief FR-29: GET /status, also served during an upload (FR-42). */
static esp_err_t HandleStatus(httpd_req_t *req)
{
    char version[FW_VERSION_FIELD_LEN];
    char body[STATUS_BODY_MAX_LEN];
    CopyInstalledVersion(version, sizeof(version));
    LockState();
    snprintf(body, sizeof(body), "state=%s&received=%u&total=%u&installed=%s", s_upload_state_names[s_upload_state],
             (unsigned)s_received_bytes, (unsigned)s_total_bytes, version);
    UnlockState();
    return SendText(req, "200 OK", body);
}

/** @brief FR-23: any other path. */
static esp_err_t HandleNotFound(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Not found");
}

/** @brief FR-18: map the embedded-metadata check of the received image head to an upload outcome. */
static upload_result_t CheckImageHead(size_t head_len)
{
    switch (CheckFwEmbeddedMeta(s_head, head_len)) {
    case FW_META_CHECK_OK:
        return UPLOAD_OK;
    case FW_META_CHECK_NO_METADATA:
        return UPLOAD_REJECT_METADATA;
    case FW_META_CHECK_BAD_VERSION:
        return UPLOAD_REJECT_VERSION;
    default:
        return UPLOAD_REJECT_IMAGE;
    }
}

/**
 * @brief Section 0.9: read and discard the rest of a rejected upload, so the browser receives the
 * FR-28 reply. No flash writes; stops on the receive-timeout limit or a closed connection.
 */
static void DiscardBody(httpd_req_t *req, size_t remaining_bytes)
{
    uint32_t timeouts = 0;
    while (remaining_bytes > 0) {
        size_t wanted_bytes = (remaining_bytes > sizeof(s_chunk)) ? sizeof(s_chunk) : remaining_bytes;
        int received = httpd_req_recv(req, (char *)s_chunk, wanted_bytes);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts < FW_UPLOAD_RECV_RETRIES) {
            ++timeouts;
            continue;
        }
        if (received <= 0) {
            return;
        }
        timeouts = 0;
        remaining_bytes -= (size_t)received;
    }
}

/**
 * @brief FR-27 steps (4) and (5): receive the body in chunks of at most FW_UPLOAD_CHUNK_BYTES, check
 * the image head as soon as it is complete, and pass each chunk to esp_ota_write().
 */
static upload_result_t ReceiveImage(httpd_req_t *req, esp_ota_handle_t handle, size_t total_bytes)
{
    size_t head_len = 0;
    size_t received_bytes = 0;
    size_t next_log_bytes = FW_UPLOAD_LOG_STEP_BYTES;
    uint32_t timeouts = 0;
    while (received_bytes < total_bytes) {
        size_t wanted_bytes = total_bytes - received_bytes;
        if (wanted_bytes > sizeof(s_chunk)) {
            wanted_bytes = sizeof(s_chunk);
        }
        int received = httpd_req_recv(req, (char *)s_chunk, wanted_bytes);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && timeouts < FW_UPLOAD_RECV_RETRIES) {
            ++timeouts;
            LOG_DEBUG("receive timeout %u of %d", (unsigned)timeouts, FW_UPLOAD_RECV_RETRIES);
            continue;
        }
        if (received <= 0) {
            return UPLOAD_REJECT_INTERRUPTED;
        }
        timeouts = 0;
        if (head_len < FW_IMAGE_HEAD_LEN) {
            size_t copy_bytes = FW_IMAGE_HEAD_LEN - head_len;
            if (copy_bytes > (size_t)received) {
                copy_bytes = (size_t)received;
            }
            memcpy(&s_head[head_len], s_chunk, copy_bytes);
            head_len += copy_bytes;
            if (head_len == FW_IMAGE_HEAD_LEN) {
                upload_result_t result = CheckImageHead(head_len);
                if (result != UPLOAD_OK) {
                    DiscardBody(req, total_bytes - received_bytes - (size_t)received);
                    return result;
                }
            }
        }
        esp_err_t err = esp_ota_write(handle, s_chunk, (size_t)received);
        if (err != ESP_OK) {
            DiscardBody(req, total_bytes - received_bytes - (size_t)received);
            /* esp_ota_write() rejects a first byte other than 0xE9 before the head is complete. */
            return (err == ESP_ERR_OTA_VALIDATE_FAILED) ? UPLOAD_REJECT_IMAGE : UPLOAD_REJECT_FLASH;
        }
        received_bytes += (size_t)received;
        LockState();
        s_received_bytes = received_bytes;
        UnlockState();
        if (received_bytes >= next_log_bytes) {
            LOG_DEBUG("received %u of %u bytes", (unsigned)received_bytes, (unsigned)total_bytes);
            next_log_bytes += FW_UPLOAD_LOG_STEP_BYTES;
        }
    }
    return (head_len < FW_IMAGE_HEAD_LEN) ? CheckImageHead(head_len) : UPLOAD_OK;
}

/** @brief FR-20: after a successful commit, clear a non-zero crash count, keeping boot_attempts. */
static void ClearCrashCount(void)
{
    fw_ctrl_record_t ctrl;
    (void)ReadFwCtrlRecord(&ctrl);
    if (ctrl.crash_count == 0) {
        return;
    }
    uint8_t crash_count = ctrl.crash_count;
    BuildFwCtrlRecord(&ctrl, 0, ctrl.boot_attempts);
    esp_err_t err = WriteFwCtrlRecord(&ctrl);
    if (err != ESP_OK) {
        LOG_WARNING("control record write failed (err=%d)", (int)err);
        return;
    }
    LOG_DEBUG("crash count %u cleared after the upload", (unsigned)crash_count);
}

/**
 * @brief FR-20: write the metadata record of the verified image into the erased sector 0 and check
 * it. If the write or the read-back fails, sector 0 is erased again so no valid record is left.
 */
static upload_result_t CommitMetaRecord(size_t total_bytes)
{
    uint8_t sha256[FW_SHA256_LEN];
    if (ComputeFwImageSha256(s_ota, sha256) != ESP_OK) {
        return UPLOAD_REJECT_FLASH;
    }
    const char *version = (const char *)&s_head[FW_EMBEDDED_META_OFFSET + offsetof(fw_embedded_meta_t, version)];
    fw_meta_record_t record;
    fw_meta_record_t read_back;
    BuildFwMetaRecord(&record, version, (uint32_t)total_bytes, sha256);
    if (WriteFwMetaRecord(&record) != ESP_OK || ReadFwMetaRecord(&read_back) != ESP_OK ||
        !IsFwMetaRecordValid(&read_back, s_ota->size) || memcmp(&record, &read_back, sizeof(record)) != 0) {
        if (EraseFwMetaRecord() != ESP_OK) {
            LOG_ERROR("metadata record erase after a failed commit failed");
        }
        return UPLOAD_REJECT_FLASH;
    }
    SetInstalledVersion(record.version);
    ClearCrashCount();
    return UPLOAD_OK;
}

/** @brief FR-27 steps (1) to (7). On return, *is_ota_open tells whether the OTA handle must be aborted. */
static upload_result_t RunUpload(httpd_req_t *req, size_t total_bytes, esp_ota_handle_t *handle, bool *is_ota_open)
{
    if (s_ota == NULL || total_bytes == 0 || total_bytes > s_ota->size) {
        return UPLOAD_REJECT_SIZE;
    }
    LOG_INFO("upload started (size=%u)", (unsigned)total_bytes);
    if (EraseFwMetaRecord() != ESP_OK) {
        return UPLOAD_REJECT_FLASH;
    }
    SetInstalledVersion(NULL);
    if (esp_ota_begin(s_ota, total_bytes, handle) != ESP_OK) {
        return UPLOAD_REJECT_FLASH;
    }
    *is_ota_open = true;
    upload_result_t result = ReceiveImage(req, *handle, total_bytes);
    if (result != UPLOAD_OK) {
        return result;
    }
    *is_ota_open = false; /* esp_ota_end() releases the handle whatever its result */
    if (esp_ota_end(*handle) != ESP_OK) {
        return UPLOAD_REJECT_VERIFY;
    }
    return CommitMetaRecord(total_bytes);
}

/** @brief FR-28: timer service callback, restart after a successful upload. */
static void HandleRestartTimer(TimerHandle_t timer)
{
    (void)timer;
    esp_restart();
}

/** @brief FR-28: restart FW_RESTART_DELAY_MS after the success response, so that it can be delivered. */
static void ScheduleRestart(void)
{
    TimerHandle_t timer = xTimerCreateStatic("restart", pdMS_TO_TICKS(FW_RESTART_DELAY_MS), pdFALSE, NULL,
                                             HandleRestartTimer, &s_restart_timer_struct);
    if (timer == NULL || xTimerStart(timer, 0) != pdPASS) {
        LOG_WARNING("restart timer not started, restarting now");
        esp_restart();
    }
}

/**
 * @brief FR-27, FR-28, FR-30: run one claimed upload in the worker task and reply. The state is
 * already UPLOADING (set by HandleUpload()). Ends the async request.
 */
static void ProcessUpload(httpd_req_t *req)
{
    size_t total_bytes = req->content_len;
    LockState();
    s_received_bytes = 0;
    s_total_bytes = total_bytes;
    UnlockState();

    esp_ota_handle_t handle = 0;
    bool is_ota_open = false;
    upload_result_t result = RunUpload(req, total_bytes, &handle, &is_ota_open);
    if (result != UPLOAD_OK) {
        if (is_ota_open) {
            (void)esp_ota_abort(handle);
        }
        SetUploadState(UPLOAD_STATE_ERROR);
        LOG_WARNING("upload rejected (reason=%s)", s_upload_replies[result].token);
        (void)SendText(req, s_upload_replies[result].status, s_upload_replies[result].body);
    } else {
        char version[FW_VERSION_FIELD_LEN];
        CopyInstalledVersion(version, sizeof(version));
        SetUploadState(UPLOAD_STATE_DONE);
        LOG_INFO("firmware %s installed (size=%u)", version, (unsigned)total_bytes);
        (void)SendText(req, s_upload_replies[UPLOAD_OK].status, s_upload_replies[UPLOAD_OK].body);
        ScheduleRestart();
    }
    (void)httpd_req_async_handler_complete(req);
}

/** @brief FR-42: upload worker task; runs one upload per queued request. */
static void RunUploadWorker(void *arg)
{
    (void)arg;
    for (;;) {
        httpd_req_t *req = NULL;
        if (xQueueReceive(s_upload_queue, &req, portMAX_DELAY) == pdTRUE && req != NULL) {
            ProcessUpload(req);
        }
    }
}

/**
 * @brief FR-27, FR-42: POST /update in the HTTP server task. Claims the upload and hands it to the
 * worker; while an upload runs or the restart is pending, answers 409 at once without reading the
 * body (ESP_FAIL makes the server close the connection instead of draining the body) and without
 * touching flash.
 */
static esp_err_t HandleUpload(httpd_req_t *req)
{
    LockState();
    bool is_busy = (s_upload_state == UPLOAD_STATE_UPLOADING || s_upload_state == UPLOAD_STATE_DONE);
    if (!is_busy) {
        s_upload_state = UPLOAD_STATE_UPLOADING;
    }
    UnlockState();
    if (is_busy) {
        LOG_DEBUG("second upload rejected (409)");
        httpd_resp_set_hdr(req, "Connection", "close");
        (void)SendText(req, "409 Conflict", "Upload in progress");
        return ESP_FAIL;
    }

    if (s_ota == NULL || req->content_len == 0 || req->content_len > s_ota->size) {
        /* Section 0.9: reject an invalid size in the server task; ESP_FAIL closes the connection
         * without draining a possibly huge body. */
        LOG_WARNING("upload rejected (reason=%s)", s_upload_replies[UPLOAD_REJECT_SIZE].token);
        SetUploadState(UPLOAD_STATE_ERROR);
        httpd_resp_set_hdr(req, "Connection", "close");
        (void)SendText(req, s_upload_replies[UPLOAD_REJECT_SIZE].status, s_upload_replies[UPLOAD_REJECT_SIZE].body);
        return ESP_FAIL;
    }

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        LOG_WARNING("upload rejected (reason=flash)");
        SetUploadState(UPLOAD_STATE_ERROR);
        /* Section 0.9: fixed text and a closed connection, so the server task does not drain the body. */
        httpd_resp_set_hdr(req, "Connection", "close");
        (void)SendText(req, s_upload_replies[UPLOAD_REJECT_FLASH].status, s_upload_replies[UPLOAD_REJECT_FLASH].body);
        return ESP_FAIL;
    }
    /* Not reachable: only one upload can be claimed, and the worker dequeues before it processes. */
    if (xQueueSend(s_upload_queue, &async_req, 0) != pdTRUE) {
        LOG_ERROR("upload hand-off to the worker failed");
        SetUploadState(UPLOAD_STATE_ERROR);
        (void)httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        (void)httpd_req_async_handler_complete(async_req);
    }
    return ESP_OK;
}

/** @brief FR-42: create the state mutex, the request queue and the upload worker (once). */
static bool StartUploadWorker(void)
{
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_struct);
    }
    if (s_upload_queue == NULL) {
        s_upload_queue = xQueueCreateStatic(1, sizeof(httpd_req_t *), s_upload_queue_storage, &s_upload_queue_struct);
        if (s_upload_queue != NULL &&
            xTaskCreateStatic(RunUploadWorker, "upload", UPDATER_UPLOAD_STACK_BYTES, NULL, UPLOAD_WORKER_PRIORITY,
                              s_worker_stack, &s_worker_struct) == NULL) {
            s_upload_queue = NULL;
        }
    }
    return s_state_mutex != NULL && s_upload_queue != NULL;
}

bool StartUpdaterHttp(updater_reason_t reason, const esp_partition_t *ota, const char *installed_version)
{
    s_reason = reason;
    s_ota = ota;
    if (!StartUploadWorker()) {
        LOG_ERROR("upload worker start failed");
        return false;
    }
    SetInstalledVersion(installed_version);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = UPDATER_HTTP_STACK_BYTES;
    config.max_open_sockets = UPDATER_HTTP_MAX_SOCKETS;
    config.max_uri_handlers = 3;
    config.lru_purge_enable = true; /* sessions holding an async request are never purged */
    if (httpd_start(&s_server, &config) != ESP_OK) {
        LOG_ERROR("HTTP server start failed");
        return false;
    }
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandlePage},
        {.uri = "/update", .method = HTTP_POST, .handler = HandleUpload},
        {.uri = "/status", .method = HTTP_GET, .handler = HandleStatus},
    };
    bool is_ok = true;
    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        is_ok = is_ok && httpd_register_uri_handler(s_server, &handlers[index]) == ESP_OK;
    }
    is_ok = is_ok && httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, HandleNotFound) == ESP_OK;
    if (!is_ok) {
        LOG_ERROR("HTTP handler registration failed");
        httpd_stop(s_server);
        s_server = NULL;
        return false;
    }
    LOG_DEBUG("HTTP server started on port %d", (int)config.server_port);
    return true;
}
