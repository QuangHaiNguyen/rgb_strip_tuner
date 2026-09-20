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
#ifdef __cplusplus
}
#endif
