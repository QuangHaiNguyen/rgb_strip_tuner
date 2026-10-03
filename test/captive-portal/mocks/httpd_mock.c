/* HTTP server simulator; see httpd_mock.h. */
#include "httpd_mock.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

/* SPEC-005 additions: request headers, error handlers, fault injection, lifecycle trace. */
#define REQUEST_HEADERS_MAX (1024)
static char s_request_headers[REQUEST_HEADERS_MAX];
static int s_recv_count;
static int s_header_read_count;
static httpd_err_handler_func_t s_err_handlers[HTTPD_ERR_CODE_MAX];
static int s_fail_registration_at;
static int s_registration_attempts;
static bool s_fail_err_handler;
static char s_lifecycle[1024];

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
    s_request_headers[0] = '\0';
    s_recv_count = 0;
    s_header_read_count = 0;
    memset(s_err_handlers, 0, sizeof(s_err_handlers));
    s_fail_registration_at = -1;
    s_registration_attempts = 0;
    s_fail_err_handler = false;
    s_lifecycle[0] = '\0';
}

static void AppendLifecycle(const char *event)
{
    if (s_lifecycle[0] != '\0') {
        strncat(s_lifecycle, ",", sizeof(s_lifecycle) - strlen(s_lifecycle) - 1);
    }
    strncat(s_lifecycle, event, sizeof(s_lifecycle) - strlen(s_lifecycle) - 1);
}

/** Remove header @p name (case-insensitive) from s_request_headers. */
static void RemoveRequestHeader(const char *name)
{
    size_t name_length = strlen(name);
    char *line = s_request_headers;
    while (*line != '\0') {
        char *end = strchr(line, '\n');
        size_t line_length = end == NULL ? strlen(line) : (size_t)(end - line) + 1;
        if (strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
            memmove(line, line + line_length, strlen(line + line_length) + 1);
        } else {
            line += line_length;
        }
    }
}

void TestHttpdSetRequestHeader(const char *name, const char *value)
{
    RemoveRequestHeader(name);
    if (value != NULL) {
        size_t used = strlen(s_request_headers);
        snprintf(s_request_headers + used, sizeof(s_request_headers) - used, "%s: %s\n", name, value);
    }
}

void TestHttpdClearRequestHeaders(void) { s_request_headers[0] = '\0'; }
int TestHttpdRecvCount(void) { return s_recv_count; }
int TestHttpdHeaderReadCount(void) { return s_header_read_count; }
httpd_err_handler_func_t TestHttpdErrHandler(httpd_err_code_t error) { return s_err_handlers[error]; }
void TestHttpdFailRegistrationAt(int index) { s_fail_registration_at = index; }
void TestHttpdFailErrHandler(bool fails) { s_fail_err_handler = fails; }
const char *TestHttpdLifecycle(void) { return s_lifecycle; }
bool TestHttpdIsRunning(void) { return s_running; }

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
    memset(s_err_handlers, 0, sizeof(s_err_handlers));   /* a new instance starts without error handlers */
    s_registration_attempts = 0;
    s_running = true;
    s_start_count++;
    AppendLifecycle("start");
    *handle = &s_config;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    (void)handle;
    s_running = false;
    s_stop_count++;
    AppendLifecycle("stop");
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    if (s_registration_attempts++ == s_fail_registration_at) {
        return ESP_FAIL;
    }
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
    request.mock_headers = s_request_headers;

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
    /* Like esp_http_server: a known URI with another method is 405, anything else 404; a registered
     * error handler for that code replaces the default response (SPEC-005 FR-14). */
    httpd_err_code_t error = HTTPD_404_NOT_FOUND;
    for (int index = 0; index < s_handler_count; ++index) {
        if (UriMatches(s_handlers[index].uri, uri)) {
            error = HTTPD_405_METHOD_NOT_ALLOWED;
        }
    }
    if (s_err_handlers[error] != NULL) {
        esp_err_t result = s_err_handlers[error](&request, error);
        for (int header = 0; header < s_header_count; ++header) {
            AppendOutput(s_headers[header][1]);
        }
        AppendOutput(s_body);
        return result;
    }
    strcpy(s_status, error == HTTPD_405_METHOD_NOT_ALLOWED ? "405 Method Not Allowed" : "404 Not Found");
    return ESP_FAIL;
}

esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error, httpd_err_handler_func_t handler)
{
    (void)handle;
    if (s_fail_err_handler || (unsigned)error >= (unsigned)HTTPD_ERR_CODE_MAX) {
        return ESP_FAIL;
    }
    s_err_handlers[error] = handler;
    return ESP_OK;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *field, char *value, size_t value_size)
{
    s_header_read_count++;
    size_t field_length = strlen(field);
    const char *line = request->mock_headers == NULL ? "" : request->mock_headers;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t line_length = end == NULL ? strlen(line) : (size_t)(end - line);
        if (strncasecmp(line, field, field_length) == 0 && line[field_length] == ':') {
            const char *start = line + field_length + 1;
            while (*start == ' ') {
                ++start;
            }
            size_t length = (size_t)(line + line_length - start);
            if (value_size == 0) {
                return ESP_ERR_HTTPD_RESULT_TRUNC;
            }
            /* esp_http_server copies what fits (null-terminated) and reports truncation. */
            size_t copied = length < value_size ? length : value_size - 1;
            memcpy(value, start, copied);
            value[copied] = '\0';
            return length < value_size ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
        }
        line += line_length + (end == NULL ? 0 : 1);
    }
    return ESP_ERR_NOT_FOUND;
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
    s_recv_count++;
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
