/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file credential_store.h
 * @brief Encrypted, transactional persistence of one Wi-Fi credential pair (SPEC-002 FR-17, §7).
 *
 * The pair is stored as a single blob in the encrypted NVS namespace `wifi_cfg`.
 * Replacement writes `cred_pending`, verifies it, copies it to `cred_active`,
 * verifies that, and only then erases `cred_pending`, so a power loss leaves
 * either the complete old pair or the complete new pair.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum SSID length in bytes, excluding the terminator. */
#define CREDENTIAL_SSID_MAX (32)
/** @brief Maximum password length in characters, excluding the terminator. */
#define CREDENTIAL_PASSWORD_MAX (64)

/** @brief One complete Wi-Fi credential record. */
typedef struct {
    char ssid[CREDENTIAL_SSID_MAX + 1];         /**< Null-terminated SSID. */
    char password[CREDENTIAL_PASSWORD_MAX + 1]; /**< Null-terminated password, empty for an open network. */
} wifi_credentials_t;

/**
 * @brief Check a credential pair against the FR-14 rules.
 *
 * The SSID must be 1..32 bytes. The password must be empty (open network),
 * 8..63 printable ASCII characters, or exactly 64 hexadecimal characters.
 *
 * @param credentials Pair to check.
 * @return true if the pair is well formed.
 */
bool IsCredentialsValid(const wifi_credentials_t *credentials);

/**
 * @brief Initialize encrypted NVS and recover an interrupted replacement.
 *
 * Fails closed (NFR-8): if encrypted NVS cannot be initialized an Error is
 * logged and the store reports no credentials.
 *
 * @return true if the encrypted store is ready.
 */
bool InitCredentialStore(void);

/**
 * @brief Read the active credential record.
 *
 * @param[out] credentials Receives the pair on success.
 * @return true if a complete, valid record was read.
 */
bool LoadCredentials(wifi_credentials_t *credentials);

/**
 * @brief Transactionally replace the stored credentials.
 *
 * On any failure the previous active record is left unchanged.
 *
 * @param credentials New pair, must satisfy IsCredentialsValid().
 * @return true if the new pair is stored and verified.
 */
bool ReplaceCredentials(const wifi_credentials_t *credentials);

#ifdef __cplusplus
}
#endif
