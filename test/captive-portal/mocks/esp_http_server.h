#pragma once
/* Host mock of esp_http_server.h: only what http_portal.c uses. */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

typedef void *httpd_handle_t;
typedef struct httpd_req {
    size_t content_len;
    char uri[512];
    int method;
    const char *mock_body;   /* test-only: request body served by httpd_req_recv() */
    size_t mock_body_pos;
} httpd_req_t;
typedef esp_err_t (*httpd_handler_t)(httpd_req_t *request);
typedef struct httpd_uri {
    const char *uri;
    int method;
    httpd_handler_t handler;
    void *user_ctx;
} httpd_uri_t;
typedef bool (*httpd_uri_match_func_t)(const char *reference_uri, const char *uri_to_match, size_t match_upto);
typedef struct {
    unsigned task_priority;
    size_t stack_size;
    uint16_t server_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    bool lru_purge_enable;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() { .task_priority = 5, .stack_size = 4096, .server_port = 80, \
                                 .max_open_sockets = 7, .max_uri_handlers = 8, .lru_purge_enable = false, \
                                 .uri_match_fn = NULL }
#define HTTPD_RESP_USE_STRLEN (-1)
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
#define HTTP_GET (1)
#define HTTP_POST (3)
#define HTTP_ANY INT_MAX
#define ESP_ERR_HTTPD_HANDLERS_FULL (0x8001)
typedef enum { HTTPD_500_INTERNAL_SERVER_ERROR = 0, HTTPD_400_BAD_REQUEST = 1 } httpd_err_code_t;
/* Status-line string constants (real esp_http_server.h section "HTTP Response"); http_portal.c's
 * tuner handlers set the status directly with httpd_resp_set_status() rather than through
 * httpd_resp_send_err(), so only the one status this project uses is added here. */
#define HTTPD_400 "400 Bad Request"

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
bool httpd_uri_match_wildcard(const char *reference_uri, const char *uri_to_match, size_t match_upto);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length);
esp_err_t httpd_resp_send_err(httpd_req_t *request, httpd_err_code_t error, const char *message);
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length);
#ifdef __cplusplus
}
#endif
