#pragma once
/**
 * @file httpd_mock.h
 * @brief Simulates the ESP-IDF HTTP server: records registrations and routes requests to handlers.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif
void TestHttpdReset(void);
void TestHttpdFailStart(bool fails);
/** The next @p count httpd_req_recv() calls return HTTPD_SOCK_ERR_TIMEOUT. */
void TestHttpdRecvTimeouts(int count);
int TestHttpdStartCount(void);
int TestHttpdStopCount(void);
const httpd_config_t *TestHttpdConfig(void);
int TestHttpdHandlerCount(void);
const char *TestHttpdUriAt(int index);
int TestHttpdMethodAt(int index);
bool TestHttpdHasUri(const char *uri);
/** Route a request like the real server (first registered match wins); returns the handler's result. */
esp_err_t TestHttpdRequest(int method, const char *uri, const char *body);
/** Response of the last request. */
const char *TestHttpdStatus(void);
const char *TestHttpdBody(void);
const char *TestHttpdHeader(const char *name);
const char *TestHttpdContentType(void);
/** All response bodies and headers since the last reset, concatenated (for leak checks). */
const char *TestHttpdAllOutput(void);

/* ---- SPEC-005 additions ---------------------------------------------------------------------------------------- */
/** Set a request header sent with every following TestHttpdRequest() (name compared case-insensitively);
 *  a NULL @p value removes it. Headers persist until TestHttpdClearRequestHeaders() or TestHttpdReset(). */
void TestHttpdSetRequestHeader(const char *name, const char *value);
void TestHttpdClearRequestHeaders(void);
/** Calls of httpd_req_recv() / httpd_req_get_hdr_value_str() since the last reset. */
int TestHttpdRecvCount(void);
int TestHttpdHeaderReadCount(void);
/** Error handler registered with httpd_register_err_handler() since the last httpd_start(), or NULL. */
httpd_err_handler_func_t TestHttpdErrHandler(httpd_err_code_t error);
/** Make the @p index-th (0-based, counted from the last httpd_start()) httpd_register_uri_handler() fail; -1 = none. */
void TestHttpdFailRegistrationAt(int index);
/** Make httpd_register_err_handler() fail. */
void TestHttpdFailErrHandler(bool fails);
/** "start"/"stop" events in call order, comma separated, since the last reset. */
const char *TestHttpdLifecycle(void);
/** True between a successful httpd_start() and the next httpd_stop(). */
bool TestHttpdIsRunning(void);
#ifdef __cplusplus
}
#endif
