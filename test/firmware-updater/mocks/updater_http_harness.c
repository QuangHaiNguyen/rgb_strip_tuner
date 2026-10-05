/* Compiles updater/main/updater_http.c into the test; see updater_http_harness.h. httpd_req_recv() of the updater is
 * redirected to a scripted receive so that timeouts, short reads and closed connections can be produced; the
 * response functions are those of the captive-portal httpd simulator. */
#include "updater_http_harness.h"
#include <string.h>
#include "httpd_mock.h"

int HarnessRecv(httpd_req_t *request, char *buffer, size_t length);
#define httpd_req_recv HarnessRecv
/* SPEC-007 FR-42 (2026-10-05): POST /update is handed to an upload worker task through a queue. The harness runs the
 * worker step synchronously inside the hand-off, so HarnessUpload() still returns after the response is sent. */
#include "freertos/queue.h"
esp_err_t HarnessAsyncBegin(httpd_req_t *request, httpd_req_t **out);
esp_err_t HarnessAsyncComplete(httpd_req_t *request);
BaseType_t HarnessQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks);
#define httpd_req_async_handler_begin HarnessAsyncBegin
#define httpd_req_async_handler_complete HarnessAsyncComplete
#define xQueueSend HarnessQueueSend
#include "../../../updater/main/updater_http.c"
#undef httpd_req_recv
#undef xQueueSend

/* FR-42 hand-off. httpd_req_async_handler_begin() gives a copy of the request (as esp_http_server does); the worker
 * step runs at once inside xQueueSend(), or, in deferred mode, only when the test calls HarnessRunWorker(). */
static httpd_req_t s_async_copy;
static size_t s_async_available;
static bool s_async_copy_in_use;
static int s_async_begin_count;
static int s_async_complete_count;
static int s_queue_send_count;
static bool s_fail_async_begin;
static bool s_fail_queue_send;
static bool s_is_worker_deferred;
static httpd_req_t *s_pending_request;
static size_t HarnessAvailableFor(const httpd_req_t *request);

esp_err_t HarnessAsyncBegin(httpd_req_t *request, httpd_req_t **out)
{
    s_async_begin_count++;
    if (s_fail_async_begin) {
        return ESP_ERR_NO_MEM;
    }
    s_async_available = HarnessAvailableFor(request);
    s_async_copy = *request;
    s_async_copy_in_use = true;
    *out = &s_async_copy;
    return ESP_OK;
}

esp_err_t HarnessAsyncComplete(httpd_req_t *request)
{
    if (request == &s_async_copy) {
        s_async_copy_in_use = false;
    }
    s_async_complete_count++;
    return ESP_OK;
}

BaseType_t HarnessQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    (void)queue;
    (void)wait_ticks;
    s_queue_send_count++;
    if (s_fail_queue_send || s_pending_request != NULL) {
        return pdFALSE;   /* one-item queue: full while a request waits */
    }
    httpd_req_t *request = *(httpd_req_t *const *)item;
    if (s_is_worker_deferred) {
        s_pending_request = request;
    } else {
        ProcessUpload(request);
    }
    return pdTRUE;
}

bool HarnessRunWorker(void)
{
    httpd_req_t *request = s_pending_request;
    if (request == NULL) {
        return false;
    }
    s_pending_request = NULL;
    ProcessUpload(request);
    return true;
}

void HarnessSetWorkerDeferred(bool deferred) { s_is_worker_deferred = deferred; }
void HarnessFailAsyncBegin(bool fail) { s_fail_async_begin = fail; }
void HarnessFailQueueSend(bool fail) { s_fail_queue_send = fail; }
int HarnessAsyncBeginCount(void) { return s_async_begin_count; }
int HarnessAsyncCompleteCount(void) { return s_async_complete_count; }
int HarnessQueueSendCount(void) { return s_queue_send_count; }
bool HarnessAsyncRequestOpen(void) { return s_async_copy_in_use; }
int HarnessMutexTakes(void) { return s_state_mutex_struct.take_count; }
int HarnessMutexGives(void) { return s_state_mutex_struct.give_count; }
uint32_t HarnessWorkerStackBytes(void) { return (uint32_t)sizeof(s_worker_stack); }
uint32_t HarnessQueueStorageBytes(void) { return (uint32_t)sizeof(s_upload_queue_storage); }

/* Minimal FreeRTOS objects for the worker task, its queue and the state mutex (no scheduler in this binary). */
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *queue)
{
    (void)length;
    (void)item_size;
    (void)storage;
    return queue;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    (void)queue;
    (void)item;
    (void)wait_ticks;
    return pdFALSE;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) { return buffer; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks)
{
    (void)wait_ticks;
    semaphore->take_count++;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    semaphore->give_count++;
    return pdTRUE;
}

static uint32_t s_created_stack_depth;
static int s_created_task_count;
static char s_created_task_name[16];

uint32_t HarnessWorkerCreatedStackDepth(void) { return s_created_stack_depth; }
int HarnessWorkerCreatedCount(void) { return s_created_task_count; }
const char *HarnessWorkerCreatedName(void) { return s_created_task_name; }

TaskHandle_t xTaskCreateStatic(TaskFunction_t task, const char *name, uint32_t stack_depth, void *arg,
                               UBaseType_t priority, StackType_t *stack, StaticTask_t *tcb)
{
    (void)task;
    s_created_task_count++;
    s_created_stack_depth = stack_depth;
    strncpy(s_created_task_name, name, sizeof(s_created_task_name) - 1);
    (void)arg;
    (void)priority;
    (void)stack;
    return tcb;
}

#define MAX_SCRIPT (64)
#define CHUNK_BODY_MAX (8192)

static int s_script[MAX_SCRIPT];
static int s_script_count;
static int s_script_pos;
static void (*s_recv_hook)(int call_index);
static int s_recv_calls;
static size_t s_max_request;
static const httpd_req_t *s_direct_request;   /* request created by HarnessUpload() */
static size_t s_direct_available;             /* body bytes it can deliver */
static char s_chunk_body[CHUNK_BODY_MAX];
static size_t s_chunk_length;
static int s_chunk_count;
static bool s_chunk_terminated = true;

void HarnessSetRecvScript(const int *actions, int count)
{
    s_script_count = count < MAX_SCRIPT ? count : MAX_SCRIPT;
    memcpy(s_script, actions, (size_t)s_script_count * sizeof(int));
    s_script_pos = 0;
}

void HarnessSetRecvHook(void (*hook)(int call_index)) { s_recv_hook = hook; }
int HarnessRecvCalls(void) { return s_recv_calls; }
size_t HarnessMaxRecvRequest(void) { return s_max_request; }

int HarnessRecv(httpd_req_t *request, char *buffer, size_t length)
{
    int call = s_recv_calls++;
    if (length > s_max_request) {
        s_max_request = length;
    }
    if (s_recv_hook != NULL) {
        s_recv_hook(call);
    }
    size_t cap = length;
    if (s_script_pos < s_script_count) {
        int action = s_script[s_script_pos++];
        if (action <= 0) {
            return action;
        }
        cap = (size_t)action;
    }
    size_t available = HarnessAvailableFor(request);
    size_t remaining = available > request->mock_body_pos ? available - request->mock_body_pos : 0;
    if (remaining == 0) {
        return 0;   /* the peer closed the connection */
    }
    size_t count = length;
    if (count > cap) {
        count = cap;
    }
    if (count > remaining) {
        count = remaining;
    }
    memcpy(buffer, request->mock_body + request->mock_body_pos, count);
    request->mock_body_pos += count;
    return (int)count;
}

static size_t HarnessAvailableFor(const httpd_req_t *request)
{
    if (request == &s_async_copy) {
        return s_async_available;
    }
    return (request == s_direct_request) ? s_direct_available : request->content_len;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    (void)request;
    if (s_chunk_terminated) {
        s_chunk_length = 0;
        s_chunk_body[0] = '\0';
        s_chunk_count = 0;
        s_chunk_terminated = false;
    }
    size_t count = (buffer == NULL) ? 0 : (length < 0 ? strlen(buffer) : (size_t)length);
    if (count == 0) {
        s_chunk_terminated = true;
        return ESP_OK;
    }
    s_chunk_count++;
    if (s_chunk_length + count < CHUNK_BODY_MAX) {
        memcpy(s_chunk_body + s_chunk_length, buffer, count);
        s_chunk_length += count;
        s_chunk_body[s_chunk_length] = '\0';
    }
    return ESP_OK;
}

const char *HarnessChunkBody(void) { return s_chunk_body; }
size_t HarnessChunkBodyLength(void) { return s_chunk_length; }
int HarnessChunkCount(void) { return s_chunk_count; }
bool HarnessChunkTerminated(void) { return s_chunk_terminated; }

bool HarnessStartUpdaterHttp(updater_reason_t reason, const char *installed_version)
{
    s_server = NULL;
    s_ota = NULL;
    s_reason = UPDATER_REASON_NO_FIRMWARE;
    memset(s_installed_version, 0, sizeof(s_installed_version));
    s_upload_state = UPLOAD_STATE_IDLE;
    s_received_bytes = 0;
    s_total_bytes = 0;
    memset(s_head, 0, sizeof(s_head));
    memset(s_chunk, 0, sizeof(s_chunk));
    s_script_count = 0;
    s_script_pos = 0;
    s_recv_hook = NULL;
    s_recv_calls = 0;
    s_max_request = 0;
    s_direct_request = NULL;
    s_direct_available = 0;
    s_chunk_length = 0;
    s_chunk_body[0] = '\0';
    s_chunk_count = 0;
    s_chunk_terminated = true;
    memset(&s_async_copy, 0, sizeof(s_async_copy));
    s_async_available = 0;
    s_async_copy_in_use = false;
    s_async_begin_count = 0;
    s_async_complete_count = 0;
    s_queue_send_count = 0;
    s_fail_async_begin = false;
    s_fail_queue_send = false;
    s_is_worker_deferred = false;
    s_pending_request = NULL;
    s_state_mutex_struct.take_count = 0;
    s_state_mutex_struct.give_count = 0;
    TestHttpdReset();
    const esp_partition_t *ota = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    return StartUpdaterHttp(reason, ota, installed_version);
}

esp_err_t HarnessUpload(const uint8_t *body, size_t body_len, size_t content_len)
{
    /* Clear the simulator's last response (status, body, headers) with a request to an unknown path. */
    (void)TestHttpdRequest(HTTP_GET, "/__harness_clear__", NULL);
    httpd_req_t request;
    memset(&request, 0, sizeof(request));
    strcpy(request.uri, "/update");
    request.method = HTTP_POST;
    request.content_len = content_len;
    request.mock_body = (const char *)body;
    const httpd_req_t *saved_request = s_direct_request;
    size_t saved_available = s_direct_available;
    s_direct_request = &request;
    s_direct_available = body_len;
    esp_err_t result = HandleUpload(&request);
    s_direct_request = saved_request;
    s_direct_available = saved_available;
    return result;
}
