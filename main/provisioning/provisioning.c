/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file provisioning.c
 * @brief Orchestrator task and state machine for Wi-Fi provisioning (SPEC-002 FR-3..FR-6, FR-15..FR-18, FR-21, FR-23).
 *
 * Components report to the orchestrator through one message queue. Timers
 * (attempt timeout, pause, reconnect delay, AP shutdown delay) are driven by the
 * queue receive timeout, so a single task owns all state. The only cross-task read, the
 * HTTP server asking whether a submission is acceptable, goes through a mutex.
 */
#include "provisioning.h"
#include "button.h"
#include "credential_store.h"
#include "dns_server.h"
#include "http_portal.h"
#include "logging.h"
#include "wifi_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOOT_ATTEMPT_MAX (5)
#define ATTEMPT_TIMEOUT_MS (10000)
#define ATTEMPT_PAUSE_MS (2000)
#define BOOT_BUTTON_WINDOW_MS (BUTTON_HOLD_MS + 50)
#define AP_SHUTDOWN_DELAY_MS (3000)
#define PORTAL_RETRY_MS (5000)
#define QUEUE_LENGTH (8)
#define TASK_STACK_BYTES (6144)
#define TASK_PRIORITY (5)

LOG_MODULE_REGISTER("provision", LOG_LEVEL_DEBUG);

/** @brief Orchestrator states. */
typedef enum {
    STATE_BOOT_WAIT,       /**< Grace window for a button press at boot. */
    STATE_STA_ATTEMPT,     /**< Boot connection attempt to the stored network. */
    STATE_STA_PAUSE,       /**< Pause between failed boot attempts. */
    STATE_CONNECTED,       /**< Associated with the configured network. */
    STATE_RECONNECT_WAIT,  /**< Waiting out the backoff delay. */
    STATE_RECONNECT_TRY,   /**< Reconnect attempt in flight. */
    STATE_PORTAL_IDLE,     /**< Provisioning mode, waiting for a submission. */
    STATE_PORTAL_TRIAL,    /**< Provisioning mode, trying submitted credentials. */
    STATE_PORTAL_SUCCESS,  /**< Provisioning mode, success shown, AP about to stop. */
    STATE_PORTAL_RETRY,    /**< Provisioning services failed to start; waiting to try again. */
} orchestrator_state_t;

/** @brief Message types accepted by the orchestrator queue. */
typedef enum {
    MSG_BUTTON_REQUEST,
    MSG_STA_CONNECTED,
    MSG_STA_DISCONNECTED,
    MSG_CREDENTIALS_SUBMITTED,
} message_type_t;

/** @brief One orchestrator queue item. */
typedef struct {
    message_type_t type;
    wifi_credentials_t credentials; /**< Valid for MSG_CREDENTIALS_SUBMITTED only. */
} message_t;

static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[QUEUE_LENGTH * sizeof(message_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_struct;
static StackType_t s_task_stack[TASK_STACK_BYTES];

static StaticSemaphore_t s_state_mutex_struct;
static SemaphoreHandle_t s_state_mutex;   /* s_state is written here and read by the HTTP server task */
static orchestrator_state_t s_state = STATE_BOOT_WAIT;
static bool s_has_deadline;
static TickType_t s_deadline_ticks;
static uint32_t s_attempt_count;
static uint32_t s_failure_count;
static bool s_is_associated;
static bool s_has_stored;
static wifi_credentials_t s_stored;
static wifi_credentials_t s_active;
static wifi_credentials_t s_trial;
static provisioning_state_cb_t s_state_cb;

void ProvisioningSetStateCallback(provisioning_state_cb_t callback)
{
    s_state_cb = callback;
}

static void NotifyState(provisioning_state_t state)
{
    if (s_state_cb != NULL) {
        s_state_cb(state);
    }
}

/** @brief Enter a state; a timeout of 0 means no deadline. */
static void SetState(orchestrator_state_t state, uint32_t timeout_ms)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_state_mutex);
    s_has_deadline = (timeout_ms != 0);
    s_deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
}

static bool IsPortalState(void)
{
    return s_state == STATE_PORTAL_IDLE || s_state == STATE_PORTAL_TRIAL || s_state == STATE_PORTAL_SUCCESS ||
           s_state == STATE_PORTAL_RETRY;
}

static void PostMessage(message_type_t type, const wifi_credentials_t *credentials)
{
    message_t message = {.type = type};
    if (credentials != NULL) {
        message.credentials = *credentials;
    }
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        LOG_WARNING("orchestrator queue full, message %d dropped", (int)type);
    }
    memset(&message, 0, sizeof(message));
}

static void HandleButtonRequest(void)
{
    PostMessage(MSG_BUTTON_REQUEST, NULL);
}

static void HandleWifiManagerEvent(wifi_manager_event_t event)
{
    PostMessage(event == WIFI_MANAGER_EVENT_STA_CONNECTED ? MSG_STA_CONNECTED : MSG_STA_DISCONNECTED, NULL);
}

static bool SubmitCredentials(const wifi_credentials_t *credentials)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool is_idle = (s_state == STATE_PORTAL_IDLE);
    xSemaphoreGive(s_state_mutex);
    if (!is_idle) {
        return false;
    }
    PostMessage(MSG_CREDENTIALS_SUBMITTED, credentials);
    return true;
}

static const http_portal_ops_t s_portal_ops = {
    .scan_networks = ScanWifiNetworks,
    .submit_credentials = SubmitCredentials,
};

static void StartBootAttempt(void)
{
    ++s_attempt_count;
    LOG_INFO("station attempt %u of %d", (unsigned)s_attempt_count, BOOT_ATTEMPT_MAX);
    (void)ConnectWifiStation(&s_stored);
    SetState(STATE_STA_ATTEMPT, ATTEMPT_TIMEOUT_MS);
}

static void BeginReconnectWait(void)
{
    uint32_t delay_ms = GetWifiReconnectDelayMs(s_failure_count);
    LOG_INFO("reconnecting in %u ms", (unsigned)delay_ms);
    SetState(STATE_RECONNECT_WAIT, delay_ms);
}

static void StopPortalServices(void)
{
    StopHttpPortal();
    StopDnsServer();
    (void)StopWifiAccessPoint();
}

static void EnterProvisioning(void)
{
    LOG_INFO("entering provisioning mode");
    DisconnectWifiStation();  /* FR-23: no background retry of the stored network */
    bool is_started = StartWifiAccessPoint() &&
                      StartDnsServer(GetWifiAccessPointAddress()) &&
                      StartHttpPortal(&s_portal_ops);
    if (!is_started) {
        /* Never sit in provisioning mode with a dead portal: undo the partial start and try again. */
        LOG_ERROR("provisioning services failed to start, retrying in %d ms", PORTAL_RETRY_MS);
        StopPortalServices();
        SetState(STATE_PORTAL_RETRY, PORTAL_RETRY_MS);
        return;
    }
    SetState(STATE_PORTAL_IDLE, 0);
    NotifyState(PROVISIONING_STATE_PORTAL);
}

static void LeaveProvisioning(void)
{
    StopPortalServices();
    memset(&s_trial, 0, sizeof(s_trial));
    LOG_INFO("provisioning mode left");
    if (s_is_associated) {
        s_failure_count = 0;
        SetState(STATE_CONNECTED, 0);
        NotifyState(PROVISIONING_STATE_CONNECTED);
    } else {
        s_failure_count = 0;
        NotifyState(PROVISIONING_STATE_DISCONNECTED);
        BeginReconnectWait();
    }
}

static void FailTrial(void)
{
    LOG_WARNING("submitted credentials failed");
    DisconnectWifiStation();
    memset(&s_trial, 0, sizeof(s_trial));
    SetHttpPortalStatus(PORTAL_STATUS_FAILED);
    SetState(STATE_PORTAL_IDLE, 0);
}

static void HandleStationConnected(void)
{
    switch (s_state) {
    case STATE_STA_ATTEMPT:
    case STATE_RECONNECT_WAIT:
    case STATE_RECONNECT_TRY:
        if (s_state == STATE_STA_ATTEMPT) {
            s_active = s_stored;
        }
        s_failure_count = 0;
        SetState(STATE_CONNECTED, 0);
        LOG_INFO("connected to configured network");
        NotifyState(PROVISIONING_STATE_CONNECTED);
        break;
    case STATE_PORTAL_TRIAL:
        if (ReplaceCredentials(&s_trial)) {
            s_active = s_trial;
            SetHttpPortalStatus(PORTAL_STATUS_CONNECTED);
            SetState(STATE_PORTAL_SUCCESS, AP_SHUTDOWN_DELAY_MS);
            LOG_INFO("new credentials connected and stored");
        } else {
            FailTrial();
        }
        break;
    default:
        break;
    }
}

static void HandleStationDisconnected(void)
{
    switch (s_state) {
    case STATE_CONNECTED:
        LOG_WARNING("connection lost");
        s_failure_count = 0;
        NotifyState(PROVISIONING_STATE_DISCONNECTED);
        BeginReconnectWait();
        break;
    case STATE_RECONNECT_TRY:
        ++s_failure_count;
        LOG_WARNING("reconnect attempt failed (%u in a row)", (unsigned)s_failure_count);
        BeginReconnectWait();
        break;
    default:
        break;  /* boot attempts and portal trials run to their own timeout */
    }
}

static void HandleMessage(const message_t *message)
{
    switch (message->type) {
    case MSG_BUTTON_REQUEST:
        if (IsPortalState()) {
            LOG_DEBUG("button request ignored: provisioning already active");
        } else {
            EnterProvisioning();
        }
        break;
    case MSG_STA_CONNECTED:
        s_is_associated = true;
        HandleStationConnected();
        break;
    case MSG_STA_DISCONNECTED:
        s_is_associated = false;
        HandleStationDisconnected();
        break;
    case MSG_CREDENTIALS_SUBMITTED:
        if (s_state == STATE_PORTAL_IDLE) {
            s_trial = message->credentials;
            LOG_INFO("trying submitted credentials");
            (void)ConnectWifiStation(&s_trial);
            SetState(STATE_PORTAL_TRIAL, ATTEMPT_TIMEOUT_MS);
        }
        break;
    }
}

static void HandleTimeout(void)
{
    switch (s_state) {
    case STATE_BOOT_WAIT:
        if (s_has_stored) {
            LOG_INFO("stored credentials found");
            NotifyState(PROVISIONING_STATE_CONNECTING);
            StartBootAttempt();
        } else {
            LOG_INFO("no stored credentials");
            EnterProvisioning();
        }
        break;
    case STATE_STA_ATTEMPT:
        LOG_WARNING("station attempt %u failed", (unsigned)s_attempt_count);
        DisconnectWifiStation();
        if (s_attempt_count < BOOT_ATTEMPT_MAX) {
            SetState(STATE_STA_PAUSE, ATTEMPT_PAUSE_MS);
        } else {
            LOG_WARNING("all boot attempts failed");
            EnterProvisioning();
        }
        break;
    case STATE_STA_PAUSE:
        StartBootAttempt();
        break;
    case STATE_RECONNECT_WAIT:
        (void)ConnectWifiStation(&s_active);
        SetState(STATE_RECONNECT_TRY, ATTEMPT_TIMEOUT_MS);
        break;
    case STATE_RECONNECT_TRY:
        ++s_failure_count;
        LOG_WARNING("reconnect attempt timed out (%u in a row)", (unsigned)s_failure_count);
        DisconnectWifiStation();
        BeginReconnectWait();
        break;
    case STATE_PORTAL_TRIAL:
        FailTrial();
        break;
    case STATE_PORTAL_SUCCESS:
        LeaveProvisioning();
        break;
    case STATE_PORTAL_RETRY:
        EnterProvisioning();
        break;
    default:
        break;
    }
}

static void RunOrchestratorTask(void *arg)
{
    (void)arg;
    SetState(STATE_BOOT_WAIT, BOOT_BUTTON_WINDOW_MS);

    for (;;) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (s_has_deadline) {
            int32_t remaining_ticks = (int32_t)(s_deadline_ticks - xTaskGetTickCount());
            wait_ticks = remaining_ticks > 0 ? (TickType_t)remaining_ticks : 0;
        }

        message_t message;
        if (xQueueReceive(s_queue, &message, wait_ticks) == pdTRUE) {
            HandleMessage(&message);
            memset(&message, 0, sizeof(message));
        } else {
            HandleTimeout();
        }
    }
}

void ProvisioningStart(void)
{
    LOG_INFO("initializing provisioning");
    s_queue = xQueueCreateStatic(QUEUE_LENGTH, sizeof(message_t), s_queue_storage, &s_queue_struct);
    s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_struct);

    (void)InitCredentialStore();
    s_has_stored = LoadCredentials(&s_stored);
    if (!InitWifiManager(HandleWifiManagerEvent)) {
        LOG_ERROR("Wi-Fi manager init failed, provisioning unavailable");
        return;
    }
    (void)StartButton(HandleButtonRequest);
    (void)xTaskCreateStatic(RunOrchestratorTask, "provisioning", TASK_STACK_BYTES, NULL,
                            TASK_PRIORITY, s_task_stack, &s_task_struct);
    LOG_INFO("provisioning started");
}
