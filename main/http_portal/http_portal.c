/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file http_portal.c
 * @brief HTTP server in two profiles: the captive portal (page, scan, submit, status,
 * tuner, tuner result and redirects) and the station-mode tuner (SPEC-005 FR-11..FR-16,
 * FR-27..FR-32).
 *
 * All request buffers are static: the ESP-IDF HTTP server runs handlers one at a
 * time in a single task, so they need no locking and keep the handler stack small.
 * The same holds for the tuner submission counter (SPEC-003 FR-23). The station
 * identity and the tuner measurement record are the exceptions: the orchestrator
 * writes them while the server task reads them, so each is guarded by its own mutex
 * (SPEC-005 FR-29, SPEC-003 FR-24).
 */
#include "http_portal.h"
#include "logging.h"
#include "mdns_service.h"
#include "tuner_page.h"
#include "ws2812_timing.h"
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define HTTP_PORTAL_MAX_URI_HANDLERS (19)  /* 18 registered plus one spare (SPEC-003 FR-6) */
#define HTTP_STATION_MAX_URI_HANDLERS (4)  /* exactly the 4 station handlers (SPEC-005 FR-11) */
/** @brief `Tuner-Seq` header value buffer: 10 digits of a uint32_t plus the terminator. */
#define TUNER_SEQ_TEXT_MAX (11)
#define HTTP_PORTAL_MAX_OPEN_SOCKETS (4)
#define HTTP_PORTAL_STACK_BYTES (5120)
#define HTTP_PORTAL_JSON_MAX (4096)
#define HTTP_PORTAL_ESCAPED_SSID_MAX (2 * 96 + 1)

LOG_MODULE_REGISTER("http_portal", LOG_LEVEL_DEBUG);

static const char s_page[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>RGB LED Tuner</title></head><body>"
    "<h1>Wi-Fi setup</h1>"
    "<p><button id=r type=button>Refresh</button> <span id=s></span></p>"
    "<form id=f><p><select id=n name=ssid size=8 required style='width:100%'></select></p>"
    "<p><input id=p name=password type=password placeholder=Password autocomplete=off></p>"
    "<p><button id=c>Connect</button></p></form><p id=m></p>"
    "<form method=get action=/tuner><button>Tuner</button></form>"
    "<script>"
    "const $=i=>document.getElementById(i);"
    "function scan(){$('s').textContent='Scanning...';$('r').disabled=true;"
    "fetch('/scan',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;return r.json()}).then(a=>{"
    "const n=$('n');n.innerHTML='';"
    "a.forEach(x=>{const o=new Option(x.ssid+' ('+x.rssi+' dBm)'+(x.open?' open':'')+"
    "(x.uns?' - not supported':''),x.ssid);o.disabled=x.uns;n.add(o)});"
    "$('s').textContent=a.length?'Select a network':'No networks found'})"
    ".catch(()=>{$('s').textContent='Scan failed, try again'}).finally(()=>{$('r').disabled=false})}"
    "function poll(){fetch('/status',{cache:'no-store'}).then(r=>r.text()).then(t=>{$('m').textContent=t;"
    "if(t==='Connecting...')setTimeout(poll,1000)}).catch(()=>setTimeout(poll,1500))}"
    "$('r').onclick=scan;"
    "$('f').onsubmit=e=>{e.preventDefault();$('m').textContent='Connecting...';"
    "fetch('/submit',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:new URLSearchParams(new FormData($('f')))})"
    ".then(r=>r.ok?poll():r.text().then(t=>{$('m').textContent=t}))"
    ".catch(()=>{$('m').textContent='Connection lost, reconnect to this network'})};"
    "scan()</script></body></html>";

/** Connectivity-check URLs handled explicitly (FR-10); everything else goes to the wildcard. */
static const char *const s_probe_uris[] = {
    "/generate_204", "/gen_204",                                 /* Android */
    "/hotspot-detect.html", "/library/test/success.html",        /* Apple */
    "/connecttest.txt", "/ncsi.txt", "/redirect",                /* Windows */
    "/canonical.html", "/success.txt", "/check_network_status.txt", /* Linux */
};

/** @brief Profile of the server instance in s_server. */
typedef enum {
    HTTP_PROFILE_NONE,         /**< No server running. */
    HTTP_PROFILE_PROVISIONING, /**< StartHttpPortal(). */
    HTTP_PROFILE_STATION,      /**< StartHttpStationServer(). */
} http_profile_t;

/** @brief Station identity for the `Origin` check (SPEC-005 FR-29). */
typedef struct {
    char hostname_in_use[MDNS_SERVICE_HOSTNAME_MAX]; /**< mDNS name without `.local`. */
    uint32_t station_ipv4;                           /**< Network byte order, 0 = unknown. */
} station_identity_t;

static httpd_handle_t s_server;
static http_profile_t s_profile;
static const http_portal_ops_t *s_ops;
static StaticSemaphore_t s_status_mutex_struct;
static SemaphoreHandle_t s_status_mutex;   /* status is written by the orchestrator and read by the server task */
static portal_status_t s_status;
static wifi_scan_entry_t s_entries[WIFI_SCAN_MAX_ENTRIES];
static uint16_t s_entry_count;
static char s_json[HTTP_PORTAL_JSON_MAX];
static char s_body[HTTP_PORTAL_FORM_MAX + 1];
static char s_tuner_body[TUNER_BODY_MAX + 1];  /* distinct from s_body (NFR-4) */
static char s_origin[HTTP_STATION_ORIGIN_MAX + 1]; /* HTTP server task only (SPEC-005 NFR-16) */
static StaticSemaphore_t s_identity_mutex_struct;
static SemaphoreHandle_t s_identity_mutex;         /* identity: written by the orchestrator, read by the server task */
static station_identity_t s_identity;
static uint32_t s_submit_seq;                      /* last issued submission number; HTTP server task only (SPEC-003 FR-23) */
static StaticSemaphore_t s_result_mutex_struct;
static SemaphoreHandle_t s_result_mutex;           /* record: written by the orchestrator, read by the server task */
static ws2812_measurement_t s_result;              /* latest published measurement (SPEC-003 FR-24) */
static char s_result_query[TUNER_RESULT_QUERY_MAX]; /* HTTP server task only (SPEC-003 NFR-4) */
static char s_result_body[TUNER_RESULT_BODY_MAX];   /* HTTP server task only (SPEC-003 NFR-4) */

void SetHttpTunerResult(const ws2812_measurement_t *measurement)
{
    if (measurement == NULL) {
        return;
    }
    if (s_result_mutex == NULL) {
        s_result_mutex = xSemaphoreCreateMutexStatic(&s_result_mutex_struct);
    }
    xSemaphoreTake(s_result_mutex, portMAX_DELAY);
    if (measurement->submit_seq >= s_result.submit_seq) {
        s_result = *measurement; /* results never move backwards */
    }
    xSemaphoreGive(s_result_mutex);
}

void SetHttpStationIdentity(const char *hostname_in_use, uint32_t station_ipv4)
{
    if (s_identity_mutex == NULL) {
        s_identity_mutex = xSemaphoreCreateMutexStatic(&s_identity_mutex_struct);
    }
    xSemaphoreTake(s_identity_mutex, portMAX_DELAY);
    memset(s_identity.hostname_in_use, 0, sizeof(s_identity.hostname_in_use));
    if (hostname_in_use != NULL) {
        strncpy(s_identity.hostname_in_use, hostname_in_use, sizeof(s_identity.hostname_in_use) - 1);
    }
    s_identity.station_ipv4 = station_ipv4;
    xSemaphoreGive(s_identity_mutex);
}

void SetHttpPortalStatus(portal_status_t status)
{
    if (s_status_mutex == NULL) {
        s_status = status;   /* server not started yet: nothing else can read it */
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status = status;
    xSemaphoreGive(s_status_mutex);
}

static portal_status_t GetHttpPortalStatus(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    portal_status_t status = s_status;
    xSemaphoreGive(s_status_mutex);
    return status;
}

static esp_err_t HandlePageRequest(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    LOG_DEBUG("serving provisioning page");
    return httpd_resp_send(request, s_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t HandleRedirectRequest(httpd_req_t *request)
{
    LOG_DEBUG("redirecting %s", request->uri);
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", HTTP_PORTAL_URL);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t HandleScanRequest(httpd_req_t *request)
{
    int count = s_ops->scan_networks(s_entries, WIFI_SCAN_MAX_ENTRIES);
    if (count < 0) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, "Scan unavailable", HTTPD_RESP_USE_STRLEN);
    }
    s_entry_count = (uint16_t)count;

    char escaped[HTTP_PORTAL_ESCAPED_SSID_MAX];
    size_t used = 0;
    s_json[used++] = '[';
    for (uint16_t index = 0; index < s_entry_count; ++index) {
        if (EscapeJsonString(s_entries[index].ssid, escaped, sizeof(escaped)) == 0) {
            continue;
        }
        int written = snprintf(&s_json[used], sizeof(s_json) - used,
                               "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s,\"uns\":%s}",
                               index > 0 ? "," : "", escaped, s_entries[index].rssi_dbm,
                               s_entries[index].is_open ? "true" : "false",
                               s_entries[index].is_unsupported ? "true" : "false");
        if (written < 0 || (size_t)written + 2 > sizeof(s_json) - used) {
            break;  /* the list is cut rather than overflowing the buffer */
        }
        used += (size_t)written;
    }
    s_json[used++] = ']';
    s_json[used] = '\0';

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_json, (ssize_t)used);
}

static esp_err_t HandleSubmitRequest(httpd_req_t *request)
{
    int length = (int)request->content_len;
    if (length <= 0 || length > HTTP_PORTAL_FORM_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
    }
    int timeouts = 0;
    for (int received = 0; received < length;) {
        int part = httpd_req_recv(request, &s_body[received], length - received);
        if (part == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= HTTP_PORTAL_RECV_RETRIES) {
            continue;   /* a slow phone: wait for the rest of the form */
        }
        if (part <= 0) {
            memset(s_body, 0, sizeof(s_body));
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form");
        }
        received += part;
    }
    s_body[length] = '\0';

    wifi_credentials_t candidate = {0};
    bool is_accepted = GetFormField(s_body, "ssid", candidate.ssid, sizeof(candidate.ssid)) &&
                       GetFormField(s_body, "password", candidate.password, sizeof(candidate.password)) &&
                       ValidateSubmission(&candidate, s_entries, s_entry_count);
    memset(s_body, 0, sizeof(s_body));
    if (!is_accepted) {
        LOG_WARNING("submitted credentials rejected");
        memset(&candidate, 0, sizeof(candidate));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid credentials");
    }

    LOG_DEBUG("credentials submitted for %s", candidate.ssid);
    bool is_started = s_ops->submit_credentials(&candidate);
    memset(&candidate, 0, sizeof(candidate));
    if (!is_started) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Busy, try again");
    }
    SetHttpPortalStatus(PORTAL_STATUS_CONNECTING);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "Connecting...", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t HandleStatusRequest(httpd_req_t *request)
{
    const char *text = "";
    switch (GetHttpPortalStatus()) {
    case PORTAL_STATUS_CONNECTING: text = "Connecting..."; break;
    case PORTAL_STATUS_CONNECTED: text = "Connected successfully"; break;
    case PORTAL_STATUS_FAILED: text = "Connection failed"; break;
    default: break;
    }
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t HandleTunerPageRequest(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    LOG_DEBUG("serving tuner page");
    return httpd_resp_send(request, g_tuner_page, HTTPD_RESP_USE_STRLEN);
}

/** @brief Send a fixed-text `400` tuner rejection (FR-18) and log the reason. */
static esp_err_t RespondTunerRejected(httpd_req_t *request, ws2812_reject_reason_t reason, const char *body)
{
    LogWs2812Rejection(reason);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, HTTPD_400);
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t HandleTunerSubmitRequest(httpd_req_t *request)
{
    int length = (int)request->content_len;
    if (length <= 0 || length > TUNER_BODY_MAX) {
        return RespondTunerRejected(request, WS2812_REJECT_MALFORMED, "Invalid request");
    }

    int timeouts = 0;
    for (int received = 0; received < length;) {
        int part = httpd_req_recv(request, &s_tuner_body[received], length - received);
        if (part == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= HTTP_PORTAL_RECV_RETRIES) {
            continue;   /* a slow phone: wait for the rest of the body */
        }
        if (part <= 0) {
            memset(s_tuner_body, 0, sizeof(s_tuner_body));
            return RespondTunerRejected(request, WS2812_REJECT_MALFORMED, "Invalid request");
        }
        received += part;
    }
    s_tuner_body[length] = '\0';

    ws2812_timing_t timing = {0};
    bool is_parsed = ParseTunerForm(s_tuner_body, &timing);
    memset(s_tuner_body, 0, sizeof(s_tuner_body));
    if (!is_parsed) {
        return RespondTunerRejected(request, WS2812_REJECT_MALFORMED, "Invalid request");
    }

    ws2812_timing_result_t result = ValidateWs2812Timing(&timing);
    if (result == WS2812_TIMING_OUT_OF_RANGE) {
        memset(&timing, 0, sizeof(timing));
        return RespondTunerRejected(request, WS2812_REJECT_OUT_OF_RANGE, "Value out of range");
    }
    if (result == WS2812_TIMING_BAD_COMBINATION) {
        memset(&timing, 0, sizeof(timing));
        return RespondTunerRejected(request, WS2812_REJECT_BAD_COMBINATION, "Invalid combination");
    }
    if (result == WS2812_TIMING_BAD_DUTY_ORDER) {
        /* SPEC-003 FR-22: never handed to led_controller, so no frame and no capture. */
        memset(&timing, 0, sizeof(timing));
        return RespondTunerRejected(request, WS2812_REJECT_BAD_DUTY_ORDER, "Bit 0 duty must be less than bit 1 duty");
    }

    LogWs2812Timing(&timing);
    uint32_t submit_seq = ++s_submit_seq; /* SPEC-003 FR-23: the first valid submission gets 1 */
    LOG_DEBUG("tuner submission seq=%u", (unsigned)submit_seq);
    s_ops->apply_led_timing(&timing, submit_seq); /* SPEC-004 FR-4: hand off before the response, never blocks. */
    memset(&timing, 0, sizeof(timing));
    char seq_text[TUNER_SEQ_TEXT_MAX];
    snprintf(seq_text, sizeof(seq_text), "%u", (unsigned)submit_seq);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Tuner-Seq", seq_text); /* seq_text outlives the send below */
    return httpd_resp_send(request, "Sent", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief SPEC-003 FR-24..FR-26: `GET /tuner/result?seq=<n>`, in both profiles.
 *
 * Copies the record under the result mutex (a NULL mutex means no result yet) and formats the
 * body after releasing it. Reads no `Origin` header and sets no CORS header (SPEC-005 FR-32).
 * A well-formed request logs nothing; a malformed one logs only a Debug line.
 */
static esp_err_t HandleTunerResultRequest(httpd_req_t *request)
{
    uint32_t request_seq = 0;
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_req_get_url_query_str(request, s_result_query, sizeof(s_result_query)) != ESP_OK ||
        !ParseTunerResultSeq(s_result_query, &request_seq)) {
        LOG_DEBUG("tuner result request malformed");
        httpd_resp_set_status(request, HTTPD_400);
        return httpd_resp_send(request, "Invalid request", HTTPD_RESP_USE_STRLEN);
    }

    ws2812_measurement_t record = {0};
    if (s_result_mutex != NULL) {
        xSemaphoreTake(s_result_mutex, portMAX_DELAY);
        record = s_result;
        xSemaphoreGive(s_result_mutex);
    }
    tuner_result_state_t state = GetTunerResultState(request_seq, s_submit_seq, &record);
    size_t length = FormatTunerResult(state, &record, s_result_body, sizeof(s_result_body));
    return httpd_resp_send(request, s_result_body, (ssize_t)length);
}

/** @brief SPEC-005 FR-12: station-profile `GET /` and `GET /tuner`, the tuner page without the Back link. */
static esp_err_t HandleStationPageRequest(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    LOG_DEBUG("serving tuner page");
    return httpd_resp_send(request, g_tuner_page_station, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief SPEC-005 FR-27..FR-29: check the request's `Origin` against the station identity.
 *
 * Reads only the header, never the body. The comparison runs while the identity mutex is held,
 * without copying the record. A header that is missing is allowed; one that does not fit
 * s_origin (longer than HTTP_STATION_ORIGIN_MAX) or cannot be read is foreign.
 */
static bool IsStationOriginAllowed(httpd_req_t *request)
{
    esp_err_t result = httpd_req_get_hdr_value_str(request, "Origin", s_origin, sizeof(s_origin));
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) {
        return false;
    }
    const char *origin = (result == ESP_OK) ? s_origin : NULL;
    if (s_identity_mutex == NULL) {
        return IsHttpStationOriginAllowed(origin, NULL, 0); /* identity never set: nothing to read */
    }
    xSemaphoreTake(s_identity_mutex, portMAX_DELAY);
    bool is_allowed = IsHttpStationOriginAllowed(origin, s_identity.hostname_in_use, s_identity.station_ipv4);
    xSemaphoreGive(s_identity_mutex);
    return is_allowed;
}

/**
 * @brief SPEC-005 FR-13, FR-28: station-profile `POST /tuner`.
 *
 * A foreign `Origin` gets a fixed `403` before any body byte is read; the Origin text is neither
 * logged nor echoed. Otherwise the request takes the unchanged provisioning-profile path.
 */
static esp_err_t HandleStationTunerSubmitRequest(httpd_req_t *request)
{
    if (!IsStationOriginAllowed(request)) {
        LOG_WARNING("tuner request rejected: reason=foreign_origin");
        httpd_resp_set_status(request, "403 Forbidden");
        httpd_resp_set_type(request, "text/plain");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, "Forbidden origin", HTTPD_RESP_USE_STRLEN);
    }
    return HandleTunerSubmitRequest(request);
}

/** @brief SPEC-005 FR-14: station-profile `404` for every path other than `/`, `/tuner` and `/tuner/result`; no redirect, no URI echo. */
static esp_err_t HandleStationNotFound(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    LOG_DEBUG("station request not found");
    httpd_resp_set_status(request, HTTPD_404);
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, "Not found", HTTPD_RESP_USE_STRLEN);
}

/** @brief Register one URI handler, logging and reporting failure (FR-6). */
static bool RegisterHttpHandler(const httpd_uri_t *handler)
{
    esp_err_t result = httpd_register_uri_handler(s_server, handler);
    if (result != ESP_OK) {
        LOG_ERROR("failed to register handler for %s (%d)", handler->uri, (int)result);
        return false;
    }
    return true;
}

bool StartHttpPortal(const http_portal_ops_t *ops)
{
    if (ops == NULL) {
        return false;
    }
    StopHttpPortal();
    if (s_status_mutex == NULL) {
        s_status_mutex = xSemaphoreCreateMutexStatic(&s_status_mutex_struct);
    }
    s_ops = ops;
    SetHttpPortalStatus(PORTAL_STATUS_IDLE);
    s_entry_count = 0;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = HTTP_PORTAL_MAX_URI_HANDLERS;
    config.max_open_sockets = HTTP_PORTAL_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.stack_size = HTTP_PORTAL_STACK_BYTES;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = NULL;
        LOG_WARNING("failed to start portal HTTP server");
        return false;
    }
    s_profile = HTTP_PROFILE_PROVISIONING;

    static const httpd_uri_t s_exact_handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandlePageRequest},
        {.uri = "/scan", .method = HTTP_GET, .handler = HandleScanRequest},
        {.uri = "/submit", .method = HTTP_POST, .handler = HandleSubmitRequest},
        {.uri = "/status", .method = HTTP_GET, .handler = HandleStatusRequest},
        /* Exact URIs, registered before the wildcard catch-all so they are never swallowed by it (FR-3). */
        {.uri = "/tuner", .method = HTTP_GET, .handler = HandleTunerPageRequest},
        {.uri = "/tuner", .method = HTTP_POST, .handler = HandleTunerSubmitRequest},
        {.uri = "/tuner/result", .method = HTTP_GET, .handler = HandleTunerResultRequest},
    };
    for (size_t index = 0; index < sizeof(s_exact_handlers) / sizeof(s_exact_handlers[0]); ++index) {
        if (!RegisterHttpHandler(&s_exact_handlers[index])) {
            StopHttpPortal();
            return false;
        }
    }
    for (size_t index = 0; index < sizeof(s_probe_uris) / sizeof(s_probe_uris[0]); ++index) {
        httpd_uri_t handler = {.uri = s_probe_uris[index], .method = HTTP_ANY, .handler = HandleRedirectRequest};
        if (!RegisterHttpHandler(&handler)) {
            StopHttpPortal();
            return false;
        }
    }
    /* Registered last so the exact URIs above win. */
    httpd_uri_t catch_all = {.uri = "/*", .method = HTTP_ANY, .handler = HandleRedirectRequest};
    if (!RegisterHttpHandler(&catch_all)) {
        StopHttpPortal();
        return false;
    }

    LOG_INFO("portal HTTP server started");
    return true;
}

bool StartHttpStationServer(const http_portal_ops_t *ops)
{
    if (ops == NULL) {
        return false;
    }
    StopHttpPortal(); /* SPEC-005 FR-7: never two profiles at once */
    s_ops = ops;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = HTTP_STATION_MAX_URI_HANDLERS;
    config.max_open_sockets = HTTP_PORTAL_MAX_OPEN_SOCKETS;
    config.lru_purge_enable = true;
    config.stack_size = HTTP_PORTAL_STACK_BYTES;
    config.uri_match_fn = NULL; /* exact matching: `/tuner/` and `/foo` fall through to the 404 handler */
    if (httpd_start(&s_server, &config) != ESP_OK) {
        s_server = NULL;
        LOG_WARNING("failed to start station HTTP server");
        return false;
    }
    s_profile = HTTP_PROFILE_STATION;

    static const httpd_uri_t s_station_handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = HandleStationPageRequest},
        {.uri = "/tuner", .method = HTTP_GET, .handler = HandleStationPageRequest},
        {.uri = "/tuner", .method = HTTP_POST, .handler = HandleStationTunerSubmitRequest},
        {.uri = "/tuner/result", .method = HTTP_GET, .handler = HandleTunerResultRequest}, /* no Origin check (FR-32) */
    };
    for (size_t index = 0; index < sizeof(s_station_handlers) / sizeof(s_station_handlers[0]); ++index) {
        if (!RegisterHttpHandler(&s_station_handlers[index])) {
            StopHttpPortal();
            return false;
        }
    }
    esp_err_t result = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, HandleStationNotFound);
    if (result != ESP_OK) {
        LOG_ERROR("failed to register the 404 handler (%d)", (int)result);
        StopHttpPortal();
        return false;
    }

    LOG_INFO("station HTTP server started");
    return true;
}

void StopHttpPortal(void)
{
    if (s_server != NULL) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        LOG_INFO("%s HTTP server stopped", s_profile == HTTP_PROFILE_STATION ? "station" : "portal");
    }
    s_profile = HTTP_PROFILE_NONE;
}
