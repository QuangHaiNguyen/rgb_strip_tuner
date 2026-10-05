#pragma once
/* The captive-portal httpd simulator header plus the inline helpers of the real esp_http_server.h that the updater
 * uses (httpd_resp_sendstr, httpd_resp_sendstr_chunk). httpd_resp_send_chunk() is implemented by
 * updater_http_harness.c, which collects the chunks into one body. */
#include "../../captive-portal/mocks/esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length);
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *str)
{
    return httpd_resp_send(request, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *request, const char *str)
{
    return httpd_resp_send_chunk(request, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}
#ifdef __cplusplus
}
#endif
