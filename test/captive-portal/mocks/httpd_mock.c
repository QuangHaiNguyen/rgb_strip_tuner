/* HTTP server simulator; see httpd_mock.h. */
#include "httpd_mock.h"
#include <stdio.h>
#include <string.h>

#define MAX_HANDLERS (32)
#define BODY_MAX (16384)

typedef struct { char uri[64]; int method; httpd_handler_t handler; } registration_t;

static registration_t s_handlers[MAX_HANDLERS];
static int s_handler_count;
static httpd_config_t s_config;
static bool s_fail_start;
static int s_recv_timeouts;
static int s_start_count;
static int s_stop_count;
static bool s_running;

static char s_status[64];
static char s_body[BODY_MAX];
static size_t s_body_length;
static char s_content_type[64];
static char s_headers[8][2][256];
static int s_header_count;
static char s_all_output[1 << 17];

void TestHttpdReset(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_count = 0;
    memset(&s_config, 0, sizeof(s_config));
    s_fail_start = false;
    s_recv_timeouts = 0;
    s_start_count = 0;
    s_stop_count = 0;
    s_running = false;
    s_status[0] = s_body[0] = s_content_type[0] = s_all_output[0] = '\0';
    s_body_length = 0;
    s_header_count = 0;
}

void TestHttpdFailStart(bool fails) { s_fail_start = fails; }
void TestHttpdRecvTimeouts(int count) { s_recv_timeouts = count; }
int TestHttpdStartCount(void) { return s_start_count; }
int TestHttpdStopCount(void) { return s_stop_count; }
const httpd_config_t *TestHttpdConfig(void) { return &s_config; }
int TestHttpdHandlerCount(void) { return s_handler_count; }
const char *TestHttpdUriAt(int index) { return s_handlers[index].uri; }
int TestHttpdMethodAt(int index) { return s_handlers[index].method; }
const char *TestHttpdStatus(void) { return s_status; }
const char *TestHttpdBody(void) { return s_body; }
const char *TestHttpdContentType(void) { return s_content_type; }
const char *TestHttpdAllOutput(void) { return s_all_output; }

bool TestHttpdHasUri(const char *uri)
{
    for (int index = 0; index < s_handler_count; ++index) {
        if (strcmp(s_handlers[index].uri, uri) == 0) {
            return true;
        }
    }
    return false;
}

const char *TestHttpdHeader(const char *name)
{
    for (int index = 0; index < s_header_count; ++index) {
        if (strcmp(s_headers[index][0], name) == 0) {
            return s_headers[index][1];
        }
    }
    return NULL;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    if (s_fail_start) {
        return ESP_FAIL;
    }
    s_config = *config;
    s_handler_count = 0;
    s_running = true;
    s_start_count++;
    *handle = &s_config;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    (void)handle;
    s_running = false;
    s_stop_count++;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    if (s_handler_count >= s_config.max_uri_handlers || s_handler_count >= MAX_HANDLERS) {
        return ESP_ERR_HTTPD_HANDLERS_FULL;
    }
    registration_t *slot = &s_handlers[s_handler_count++];
    strncpy(slot->uri, uri_handler->uri, sizeof(slot->uri) - 1);
    slot->method = uri_handler->method;
    slot->handler = uri_handler->handler;
    return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *reference_uri, const char *uri_to_match, size_t match_upto)
{
    (void)reference_uri; (void)uri_to_match; (void)match_upto;
    return false;   /* the simulator does its own matching in TestHttpdRequest() */
}

static void AppendOutput(const char *text)
{
    strncat(s_all_output, text, sizeof(s_all_output) - strlen(s_all_output) - 1);
    strncat(s_all_output, "\n", sizeof(s_all_output) - strlen(s_all_output) - 1);
}

static bool UriMatches(const char *pattern, const char *uri)
{
    char path[512];
    strncpy(path, uri, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *query = strchr(path, '?');
    if (query != NULL) {
        *query = '\0';
    }
    size_t pattern_length = strlen(pattern);
    if (pattern_length >= 2 && strcmp(pattern + pattern_length - 2, "/*") == 0) {
        return strncmp(pattern, path, pattern_length - 1) == 0;
    }
    return strcmp(pattern, path) == 0;
}

esp_err_t TestHttpdRequest(int method, const char *uri, const char *body)
{
    httpd_req_t request;
    memset(&request, 0, sizeof(request));
    strncpy(request.uri, uri, sizeof(request.uri) - 1);
    request.method = method;
    request.mock_body = body;
    request.content_len = body == NULL ? 0 : strlen(body);

    strcpy(s_status, "200 OK");
    s_body[0] = '\0';
    s_body_length = 0;
    s_content_type[0] = '\0';
    s_header_count = 0;

    for (int index = 0; index < s_handler_count; ++index) {
        registration_t *entry = &s_handlers[index];
        if ((entry->method == HTTP_ANY || entry->method == method) && UriMatches(entry->uri, uri)) {
            esp_err_t result = entry->handler(&request);
            for (int header = 0; header < s_header_count; ++header) {
                AppendOutput(s_headers[header][1]);
            }
            AppendOutput(s_body);
            return result;
        }
    }
    strcpy(s_status, "404 Not Found");
    return ESP_FAIL;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    (void)request;
    strncpy(s_status, status, sizeof(s_status) - 1);
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    strncpy(s_content_type, type, sizeof(s_content_type) - 1);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    (void)request;
    if (s_header_count < 8) {
        strncpy(s_headers[s_header_count][0], field, 255);
        strncpy(s_headers[s_header_count][1], value, 255);
        s_header_count++;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    (void)request;
    size_t count = buffer == NULL ? 0 : (length < 0 ? strlen(buffer) : (size_t)length);
    if (count >= BODY_MAX) {
        count = BODY_MAX - 1;
    }
    if (buffer != NULL) {
        memcpy(s_body, buffer, count);
    }
    s_body[count] = '\0';
    s_body_length = count;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, httpd_err_code_t error, const char *message)
{
    (void)request;
    strcpy(s_status, error == HTTPD_400_BAD_REQUEST ? "400 Bad Request" : "500 Internal Server Error");
    snprintf(s_body, sizeof(s_body), "%s", message == NULL ? "" : message);
    s_body_length = strlen(s_body);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length)
{
    if (s_recv_timeouts > 0) {
        s_recv_timeouts--;
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    size_t remaining = request->content_len - request->mock_body_pos;
    size_t count = length < remaining ? length : remaining;
    memcpy(buffer, request->mock_body + request->mock_body_pos, count);
    request->mock_body_pos += count;
    return (int)count;
}
