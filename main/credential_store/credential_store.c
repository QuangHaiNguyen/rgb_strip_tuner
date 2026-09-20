/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file credential_store.c
 * @brief Transactional encrypted NVS credential storage (SPEC-002 FR-17, NFR-1, NFR-3, NFR-8).
 */
#include "credential_store.h"
#include "logging.h"
#include <string.h>
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORE_NAMESPACE "wifi_cfg"
#define ACTIVE_KEY "cred_active"
#define PENDING_KEY "cred_pending"
#define PASSWORD_MIN_LENGTH (8)
#define PASSWORD_MAX_ASCII_LENGTH (63)

LOG_MODULE_REGISTER("cred_store", LOG_LEVEL_DEBUG);

static bool s_ready;

static bool IsPasswordValid(const char *password)
{
    size_t length = strnlen(password, CREDENTIAL_PASSWORD_MAX + 1);
    if (length == 0) {
        return true;
    }
    if (length > CREDENTIAL_PASSWORD_MAX) {
        return false;
    }
    bool is_hex = (length == CREDENTIAL_PASSWORD_MAX);
    for (size_t index = 0; index < length; ++index) {
        char character = password[index];
        if (character < 0x20 || character > 0x7E) {
            return false;
        }
        bool is_hex_digit = (character >= '0' && character <= '9') ||
                            (character >= 'a' && character <= 'f') ||
                            (character >= 'A' && character <= 'F');
        is_hex = is_hex && is_hex_digit;
    }
    if (length == CREDENTIAL_PASSWORD_MAX) {
        return is_hex;
    }
    return length >= PASSWORD_MIN_LENGTH && length <= PASSWORD_MAX_ASCII_LENGTH;
}

bool IsCredentialsValid(const wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return false;
    }
    size_t ssid_length = strnlen(credentials->ssid, sizeof(credentials->ssid));
    if (ssid_length == 0 || ssid_length > CREDENTIAL_SSID_MAX) {
        return false;
    }
    return IsPasswordValid(credentials->password);
}

/** @brief Write one record, commit it, and read it back for comparison. */
static bool WriteAndVerifyRecord(nvs_handle_t handle, const char *key, const wifi_credentials_t *credentials)
{
    esp_err_t err = nvs_set_blob(handle, key, credentials, sizeof(*credentials));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        LOG_WARNING("failed to write %s: %s (%d)", key, esp_err_to_name(err), err);
        return false;
    }

    wifi_credentials_t readback = {0};
    size_t size_bytes = sizeof(readback);
    err = nvs_get_blob(handle, key, &readback, &size_bytes);
    bool matches = (err == ESP_OK) && size_bytes == sizeof(readback) &&
                   memcmp(&readback, credentials, sizeof(readback)) == 0;
    memset(&readback, 0, sizeof(readback));
    if (!matches) {
        LOG_WARNING("verification of %s failed (%d)", key, err);
    }
    return matches;
}

/** @brief Apply the boot recovery rule of SPEC-002 §7 to a leftover pending record. */
static void RecoverPendingRecord(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        LOG_DEBUG("no credential namespace to recover");
        return;
    }

    wifi_credentials_t pending = {0};
    size_t size_bytes = sizeof(pending);
    esp_err_t err = nvs_get_blob(handle, PENDING_KEY, &pending, &size_bytes);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        LOG_DEBUG("no pending credential record");
    } else if (err == ESP_OK && size_bytes == sizeof(pending) && IsCredentialsValid(&pending)) {
        LOG_INFO("recovering pending credential record");
        if (WriteAndVerifyRecord(handle, ACTIVE_KEY, &pending)) {
            (void)nvs_erase_key(handle, PENDING_KEY);
            (void)nvs_commit(handle);
        }
    } else {
        LOG_WARNING("discarding incomplete pending credential record (%d)", err);
        (void)nvs_erase_key(handle, PENDING_KEY);
        (void)nvs_commit(handle);
    }
    memset(&pending, 0, sizeof(pending));
    nvs_close(handle);
}

bool InitCredentialStore(void)
{
    /* With CONFIG_NVS_ENCRYPTION this initializes NVS with the configured key scheme. */
    esp_err_t err = nvs_flash_init();
    s_ready = (err == ESP_OK);
    if (!s_ready) {
        LOG_ERROR("encrypted NVS init failed: %s (%d), credentials unavailable", esp_err_to_name(err), err);
        return false;
    }
    LOG_INFO("encrypted NVS ready");
    RecoverPendingRecord();
    return true;
}

bool LoadCredentials(wifi_credentials_t *credentials)
{
    if (!s_ready || credentials == NULL) {
        LOG_DEBUG("load skipped: store not ready or output buffer missing");
        return false;
    }
    memset(credentials, 0, sizeof(*credentials));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        LOG_INFO("no stored credentials (%d)", err);
        return false;
    }
    size_t size_bytes = sizeof(*credentials);
    err = nvs_get_blob(handle, ACTIVE_KEY, credentials, &size_bytes);
    nvs_close(handle);

    if (err != ESP_OK || size_bytes != sizeof(*credentials) || !IsCredentialsValid(credentials)) {
        LOG_WARNING("active credential record unavailable or invalid (%d)", err);
        memset(credentials, 0, sizeof(*credentials));
        return false;
    }
    LOG_DEBUG("loaded stored credentials for %s", credentials->ssid);
    return true;
}

bool ReplaceCredentials(const wifi_credentials_t *credentials)
{
    if (!s_ready || !IsCredentialsValid(credentials)) {
        LOG_WARNING("replace rejected: invalid credentials or store not ready");
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_WARNING("failed to open credential namespace: %s (%d)", esp_err_to_name(err), err);
        return false;
    }

    bool is_stored = WriteAndVerifyRecord(handle, PENDING_KEY, credentials) &&
                     WriteAndVerifyRecord(handle, ACTIVE_KEY, credentials);
    if (is_stored) {
        (void)nvs_erase_key(handle, PENDING_KEY);
        (void)nvs_commit(handle);
        LOG_INFO("stored credentials updated");
    } else {
        LOG_WARNING("stored credentials update failed");
    }
    nvs_close(handle);
    return is_stored;
}
