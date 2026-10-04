/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file provisioning.c
 * @brief Orchestrator task and state machine for Wi-Fi provisioning (SPEC-002 FR-3..FR-6, FR-15..FR-18, FR-21, FR-23)
 * and the station-services lifecycle (SPEC-005 FR-3..FR-10, FR-29).
 *
 * Components report to the orchestrator through one message queue. Timers
 * (attempt timeout, pause, reconnect delay, AP shutdown delay, station-service retry,
 * mDNS hostname check) are driven by the queue receive timeout, so a single task owns
 * all state. The only cross-task read, the HTTP server asking whether a submission is
 * acceptable, goes through a mutex.
 *
 * Tuner data path (SPEC-004 FR-5/FR-6, FR-34): valid timing sets arrive from the HTTP server
 * task as MSG_LED_TIMING_SUBMITTED and are handed to led_controller; pulse measurement results
 * arrive from rmt_pulse_monitor's callback as MSG_PULSE_RESULT and are stored in http_portal.
 * Read requests (SPEC-006 FR-10) arrive from the HTTP server task as MSG_PULSE_READ_REQUESTED
 * and arm a read capture with ArmPulseRead(); they never reach led_controller (SPEC-006 FR-9).
 */
#include "provisioning.h"
#include "button.h"
#include "credential_store.h"
#include "dns_server.h"
#include "http_portal.h"
#include "led_controller.h"
#include "logging.h"
#include "mdns_service.h"
#include "rmt_pulse_monitor.h"
#include "wifi_manager.h"
#include <string.h>
#include "esp_system.h"
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
#define MDNS_HOSTNAME_CHECK_DELAY_MS (3000) /* SPEC-005 FR-8: 300 ticks at 100 Hz */
#define STATION_SERVICE_RETRY_MS (5000)     /* SPEC-005 FR-9: 500 ticks at 100 Hz */
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
    MSG_LED_TIMING_SUBMITTED, /**< SPEC-004 FR-5: a valid POST /tuner submission. */
    MSG_STA_GOT_IP,           /**< SPEC-005 FR-1: the station interface got an IPv4 address. */
    MSG_PULSE_RESULT,         /**< SPEC-004 FR-34: a published pulse measurement result. */
    MSG_PULSE_READ_REQUESTED, /**< SPEC-006 FR-10: a POST /tuner/read request. */
} message_type_t;

/** @brief One orchestrator queue item. */
typedef struct {
    message_type_t type;
    union {
        wifi_credentials_t credentials;   /**< Valid for MSG_CREDENTIALS_SUBMITTED only. */
        led_request_t led_request;        /**< Valid for MSG_LED_TIMING_SUBMITTED only (SPEC-004 NFR-5). */
        ws2812_measurement_t measurement; /**< Valid for MSG_PULSE_RESULT only (SPEC-004 NFR-5). */
        uint32_t submit_seq;              /**< Valid for MSG_PULSE_READ_REQUESTED only (SPEC-006 FR-10). */
    };
} message_t;

/* SPEC-004 NFR-5, SPEC-006 FR-10: the credentials stay the largest payload, so the tuner members never grow message_t. */
_Static_assert(sizeof(ws2812_measurement_t) <= sizeof(wifi_credentials_t), "measurement payload grows message_t");
_Static_assert(sizeof(led_request_t) <= sizeof(wifi_credentials_t), "LED request payload grows message_t");

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
static bool s_has_ip;                /* SPEC-005 FR-3 */
static bool s_is_http_up;            /* station-profile HTTP server running */
static bool s_is_mdns_up;            /* mDNS responder running */
static uint32_t s_http_start_failures;  /* consecutive, for the FR-9 Error/Warning choice */
static uint32_t s_mdns_start_failures;
static char s_hostname_in_use[MDNS_SERVICE_HOSTNAME_MAX]; /* last name reported to the station identity */

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

/** @brief SPEC-005 FR-1: map each wifi_manager event explicitly; got-IP is never a disconnect. */
static void HandleWifiManagerEvent(wifi_manager_event_t event)
{
    switch (event) {
    case WIFI_MANAGER_EVENT_STA_CONNECTED:
        PostMessage(MSG_STA_CONNECTED, NULL);
        break;
    case WIFI_MANAGER_EVENT_STA_DISCONNECTED:
        PostMessage(MSG_STA_DISCONNECTED, NULL);
        break;
    case WIFI_MANAGER_EVENT_STA_GOT_IP:
        PostMessage(MSG_STA_GOT_IP, NULL);
        break;
    }
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

/**
 * @brief SPEC-004 FR-5: post a validated WS2812 timing set and its submission number onto the orchestrator queue.
 *
 * Same non-blocking, drop-and-warn pattern as PostMessage() (FR-5); a distinct
 * function is used because the queue item's payload type differs from
 * PostMessage()'s wifi_credentials_t parameter.
 *
 * @param[in] timing     Validated timing set.
 * @param[in] submit_seq Submission sequence number (SPEC-003 FR-23).
 */
static void SubmitLedTiming(const ws2812_timing_t *timing, uint32_t submit_seq)
{
    if (timing == NULL) {
        return; /* nothing to apply: never post an all-zero timing set */
    }
    message_t message = {.type = MSG_LED_TIMING_SUBMITTED};
    message.led_request.timing = *timing;
    message.led_request.submit_seq = submit_seq;
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        LOG_WARNING("orchestrator queue full, message %d dropped", (int)MSG_LED_TIMING_SUBMITTED);
    }
    memset(&message, 0, sizeof(message));
}

/**
 * @brief SPEC-004 FR-34: rmt_pulse_monitor result callback; posts the measurement as MSG_PULSE_RESULT.
 *
 * Runs in the pulse monitor's decode task, in led_controller's driver task, or in this orchestrator task
 * (ArmPulseRead() publishing not_measured, SPEC-006 FR-11). Non-blocking:
 * a full queue drops the result with a Warning, as PostMessage() does.
 *
 * @param[in] measurement Published measurement result.
 */
static void PostPulseResult(const ws2812_measurement_t *measurement)
{
    if (measurement == NULL) {
        return;
    }
    message_t message = {.type = MSG_PULSE_RESULT};
    message.measurement = *measurement;
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        LOG_WARNING("orchestrator queue full, message %d dropped", (int)MSG_PULSE_RESULT);
    }
}

/**
 * @brief SPEC-006 FR-10: post a Read request and its submission number onto the orchestrator queue.
 *
 * Runs in the HTTP server task. Non-blocking: a full queue drops the request with a Warning, as
 * PostMessage() does; nothing is published for it, so the page's poll ends without a result. Posts
 * directly, like SubmitLedTiming(), because PostMessage() only carries a wifi_credentials_t payload.
 *
 * @param[in] submit_seq Submission sequence number of the Read request (SPEC-003 FR-23).
 */
static void RequestPulseRead(uint32_t submit_seq)
{
    message_t message = {.type = MSG_PULSE_READ_REQUESTED};
    message.submit_seq = submit_seq;
    if (xQueueSend(s_queue, &message, 0) != pdTRUE) {
        LOG_WARNING("orchestrator queue full, message %d dropped", (int)MSG_PULSE_READ_REQUESTED);
    }
}

static const http_portal_ops_t s_portal_ops = {
    .scan_networks = ScanWifiNetworks,
    .submit_credentials = SubmitCredentials,
    .apply_led_timing = SubmitLedTiming,
    .request_pulse_read = RequestPulseRead,
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

/**
 * @brief SPEC-005 FR-9: log one station-service start result, Error first and Warning for repeats.
 *
 * @param service       `http` or `mdns`.
 * @param is_started    Result of the start call.
 * @param failure_count Consecutive failures of this service; reset on success.
 */
static void ReportStationServiceStart(const char *service, bool is_started, uint32_t *failure_count)
{
    if (is_started) {
        *failure_count = 0;
        return;
    }
    if (++*failure_count == 1) {
        LOG_ERROR("station service start failed (service=%s)", service);
    } else {
        LOG_WARNING("station service start failed (service=%s)", service);
    }
}

/**
 * @brief SPEC-005 FR-4, FR-8..FR-10: start whichever station service is not running.
 *
 * The identity is set before the HTTP server starts (FR-29 a). A failure of one service does not
 * skip the other. With both running, the hostname check is armed; otherwise a retry is armed.
 * Must be called in STATE_CONNECTED with an IP.
 */
static void StartStationServices(void)
{
    if (!s_is_http_up) {
        strncpy(s_hostname_in_use, MDNS_SERVICE_HOSTNAME, sizeof(s_hostname_in_use) - 1);
        SetHttpStationIdentity(s_hostname_in_use, GetWifiStationAddress());
        s_is_http_up = StartHttpStationServer(&s_portal_ops);
        ReportStationServiceStart("http", s_is_http_up, &s_http_start_failures);
    }
    if (!s_is_mdns_up) {
        s_is_mdns_up = StartMdnsService();
        ReportStationServiceStart("mdns", s_is_mdns_up, &s_mdns_start_failures);
    }
    LOG_DEBUG("station services: http=%s mdns=%s free_heap=%u min_free_heap=%u tasks=%u",
              s_is_http_up ? "up" : "down", s_is_mdns_up ? "up" : "down",
              (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
              (unsigned)uxTaskGetNumberOfTasks());
    SetState(STATE_CONNECTED, (s_is_http_up && s_is_mdns_up) ? MDNS_HOSTNAME_CHECK_DELAY_MS : STATION_SERVICE_RETRY_MS);
}

/** @brief SPEC-005 FR-6: stop mDNS, then the station HTTP server; first action on entering provisioning. */
static void StopStationServices(void)
{
    bool was_running = s_is_http_up || s_is_mdns_up;
    StopMdnsService();
    StopHttpPortal();
    s_is_http_up = false;
    s_is_mdns_up = false;
    s_http_start_failures = 0;
    s_mdns_start_failures = 0;
    if (was_running) {
        LOG_INFO("station services stopped");
    }
}

/** @brief SPEC-005 FR-21, FR-29 c: log the mDNS host name in use and pass it to the station identity. */
static void CheckMdnsHostname(void)
{
    if (LogMdnsHostnameInUse(s_hostname_in_use, sizeof(s_hostname_in_use))) {
        SetHttpStationIdentity(s_hostname_in_use, GetWifiStationAddress());
    }
}

/** @brief SPEC-005 FR-3, FR-4 a, FR-8, FR-29 b: got-IP starts or refreshes the station services in STATE_CONNECTED only. */
static void HandleStationGotIp(void)
{
    s_has_ip = true;
    if (s_state != STATE_CONNECTED) {
        LOG_DEBUG("got IP in state %d, station services not started", (int)s_state);
        return;
    }
    if (s_is_http_up) {
        SetHttpStationIdentity(s_hostname_in_use, GetWifiStationAddress()); /* the address may have changed */
    }
    if (s_is_http_up && s_is_mdns_up) {
        SetState(STATE_CONNECTED, MDNS_HOSTNAME_CHECK_DELAY_MS); /* re-arm the hostname check only */
    } else {
        StartStationServices();
    }
}

static void StopPortalServices(void)
{
    StopHttpPortal();
    StopDnsServer();
    (void)StopWifiAccessPoint();
}

static void EnterProvisioning(void)
{
    StopStationServices(); /* SPEC-005 FR-6: before any other action, for every trigger */
    s_has_ip = false;
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
        if (s_has_ip) {
            StartStationServices(); /* SPEC-005 FR-4 b: after StopPortalServices() has returned */
        }
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
        s_has_ip = false;   /* SPEC-005 FR-3; the station services keep running (FR-5) */
        HandleStationDisconnected();
        break;
    case MSG_STA_GOT_IP:
        HandleStationGotIp();
        break;
    case MSG_CREDENTIALS_SUBMITTED:
        if (s_state == STATE_PORTAL_IDLE) {
            s_trial = message->credentials;
            LOG_INFO("trying submitted credentials");
            (void)ConnectWifiStation(&s_trial);
            SetState(STATE_PORTAL_TRIAL, ATTEMPT_TIMEOUT_MS);
        }
        break;
    case MSG_LED_TIMING_SUBMITTED:
        /* Accepted in every orchestrator state (SPEC-004 FR-6), unlike MSG_CREDENTIALS_SUBMITTED. */
        ApplyWs2812Timing(&message->led_request.timing, message->led_request.submit_seq);
        break;
    case MSG_PULSE_RESULT:
        /* Accepted in every orchestrator state, whichever server profile runs (SPEC-003 FR-24). */
        SetHttpTunerResult(&message->measurement);
        break;
    case MSG_PULSE_READ_REQUESTED:
        /* Accepted in every orchestrator state; arms a capture only, generates nothing (SPEC-006 FR-9, FR-10). */
        ArmPulseRead(message->submit_seq);
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
    case STATE_CONNECTED:
        /* SPEC-005 FR-9: retry a failed station service, otherwise run the FR-21 hostname check. */
        if (!(s_is_http_up && s_is_mdns_up) && s_has_ip) {
            StartStationServices();
        } else {
            SetState(STATE_CONNECTED, 0);
            if (s_is_http_up && s_is_mdns_up) {
                CheckMdnsHostname();
            }
        }
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
    SetPulseResultCallback(PostPulseResult); /* SPEC-004 FR-34: after the queue exists, before the task */

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
