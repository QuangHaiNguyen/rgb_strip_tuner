/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file http_portal.h
 * @brief Captive-portal HTTP server and form helpers (SPEC-002 FR-10..FR-14, FR-22).
 *
 * The server serves the provisioning page, a scan endpoint, a submit endpoint
 * and a plain-text status endpoint, and redirects every other request (including
 * the OS connectivity probes) to the page. Wi-Fi access is injected through
 * http_portal_ops_t so the component does not depend on the Wi-Fi driver.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "credential_store.h"
#include "wifi_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Portal address used for redirects (the SoftAP address). */
#define HTTP_PORTAL_URL "http://192.168.4.1/"
/** @brief Times a slow form upload is retried after a receive timeout. */
#define HTTP_PORTAL_RECV_RETRIES (3)
/** @brief Maximum accepted size of a submitted form body, in bytes. */
#define HTTP_PORTAL_FORM_MAX (384)

/** @brief Connection result reported by the status endpoint (FR-22). */
typedef enum {
    PORTAL_STATUS_IDLE = 0,      /**< No submission yet; the endpoint returns an empty body. */
    PORTAL_STATUS_CONNECTING,    /**< "Connecting..." */
    PORTAL_STATUS_CONNECTED,     /**< "Connected successfully" */
    PORTAL_STATUS_FAILED,        /**< "Connection failed" */
} portal_status_t;

/** @brief Services the portal needs from its owner. */
typedef struct {
    /** Scan networks; returns the entry count or a negative value if busy/failed. */
    int (*scan_networks)(wifi_scan_entry_t *entries, uint16_t max_entries);
    /** Hand validated credentials over for a connection trial; false if one is already running. */
    bool (*submit_credentials)(const wifi_credentials_t *credentials);
} http_portal_ops_t;

/**
 * @brief Decode one application/x-www-form-urlencoded value.
 *
 * @param[in]  input        Encoded text (not null-terminated).
 * @param[in]  input_length Length of @p input.
 * @param[out] output       Receives the null-terminated decoded text.
 * @param[in]  output_size  Size of @p output.
 * @return true on success; false on a bad escape or if @p output is too small.
 */
bool DecodeFormValue(const char *input, size_t input_length, char *output, size_t output_size);

/**
 * @brief Extract and decode one field from a urlencoded form body.
 *
 * @param[in]  body       Null-terminated body.
 * @param[in]  name       Field name.
 * @param[out] value      Receives the decoded value.
 * @param[in]  value_size Size of @p value.
 * @return true if the field exists and decodes.
 */
bool GetFormField(const char *body, const char *name, char *value, size_t value_size);

/**
 * @brief Escape text for embedding in a JSON string that is later rendered as HTML.
 *
 * Quotes, backslashes, control characters and `<`, `>`, `&`, `'` become `\uXXXX` or `\X`
 * escapes, so a hostile SSID can neither break the JSON nor inject markup.
 *
 * @param[in]  input       Null-terminated text.
 * @param[out] output      Receives the null-terminated escaped text.
 * @param[in]  output_size Size of @p output.
 * @return Escaped length, or 0 if @p output is too small.
 */
size_t EscapeJsonString(const char *input, char *output, size_t output_size);

/**
 * @brief Validate a submission against the last scan result (FR-13, FR-14).
 *
 * Rejects malformed credentials, unsupported networks (enterprise, WEP, ...), and an empty password for
 * a secured or unlisted network. An open network is accepted only with an empty password.
 *
 * @param[in] credentials Submitted pair.
 * @param[in] entries     Last scan result.
 * @param[in] count       Number of entries.
 * @return true if a connection attempt may be started.
 */
bool ValidateSubmission(const wifi_credentials_t *credentials, const wifi_scan_entry_t *entries, uint16_t count);

/**
 * @brief Start the HTTP server on port 80.
 *
 * @param ops Services provided by the owner; must outlive the server.
 * @return true if the server started.
 */
bool StartHttpPortal(const http_portal_ops_t *ops);

/** @brief Stop the HTTP server. Safe to call when stopped. */
void StopHttpPortal(void);

/**
 * @brief Set the result served by the status endpoint.
 *
 * @param status New status.
 */
void SetHttpPortalStatus(portal_status_t status);

#ifdef __cplusplus
}
#endif
