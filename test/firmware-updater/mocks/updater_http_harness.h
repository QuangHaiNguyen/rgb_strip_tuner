#pragma once
/**
 * @file updater_http_harness.h
 * @brief Test access to updater/main/updater_http.c (compiled into updater_http_harness.c): reset of its file-scope
 *        state, direct POST /update requests with a binary body and an arbitrary Content-Length, a scripted
 *        httpd_req_recv() and the chunked page body.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "updater_page.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Recv script actions: a positive value delivers at most that many bytes; 0 = connection closed; -1 = socket error;
 *  HTTPD_SOCK_ERR_TIMEOUT (-3) = receive timeout. After the script, every call delivers min(requested, remaining). */
void HarnessSetRecvScript(const int *actions, int count);
/** Hook run at the start of every scripted receive, with its 0-based call index (NULL removes it). */
void HarnessSetRecvHook(void (*hook)(int call_index));
int HarnessRecvCalls(void);
size_t HarnessMaxRecvRequest(void);

/** Reset the updater HTTP state, the httpd simulator and the recv script, then call StartUpdaterHttp(). */
bool HarnessStartUpdaterHttp(updater_reason_t reason, const char *installed_version);
/** POST /update straight to the upload handler: @p body_len bytes are available, Content-Length is @p content_len. */
esp_err_t HarnessUpload(const uint8_t *body, size_t body_len, size_t content_len);

/* ---- FR-42 worker hand-off (T-19) ---- */
/** Deferred mode: xQueueSend() only stores the request; HarnessRunWorker() then runs the worker step. */
void HarnessSetWorkerDeferred(bool deferred);
/** Run the worker step for the queued request; false if none is queued. */
bool HarnessRunWorker(void);
/** Make httpd_req_async_handler_begin() / xQueueSend() fail. */
void HarnessFailAsyncBegin(bool fail);
void HarnessFailQueueSend(bool fail);
int HarnessAsyncBeginCount(void);
int HarnessAsyncCompleteCount(void);
int HarnessQueueSendCount(void);
/** True between a successful async begin and its complete. */
bool HarnessAsyncRequestOpen(void);
/** xSemaphoreTake / xSemaphoreGive calls on the upload-state mutex since HarnessStartUpdaterHttp(). */
int HarnessMutexTakes(void);
int HarnessMutexGives(void);
/** Sizes of the static worker stack and queue storage. */
uint32_t HarnessWorkerStackBytes(void);
uint32_t HarnessQueueStorageBytes(void);
/** Arguments of the xTaskCreateStatic() call that created the worker (once per process). */
uint32_t HarnessWorkerCreatedStackDepth(void);
int HarnessWorkerCreatedCount(void);
const char *HarnessWorkerCreatedName(void);

/** Body collected from httpd_resp_send_chunk() for the last chunked response. */
const char *HarnessChunkBody(void);
size_t HarnessChunkBodyLength(void);
int HarnessChunkCount(void);
bool HarnessChunkTerminated(void);
#ifdef __cplusplus
}
#endif
