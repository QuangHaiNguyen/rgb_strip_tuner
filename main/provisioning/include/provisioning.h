/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file provisioning.h
 * @brief Wi-Fi provisioning orchestrator (SPEC-002).
 *
 * Owns the boot decision (button, stored credentials, captive portal), the
 * provisioning mode, and reconnect after a connection loss. The components
 * `button`, `credential_store`, `wifi_manager`, `dns_server` and `http_portal`
 * are coordinated through one task and one message queue.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Connection state reported to the application. */
typedef enum {
    PROVISIONING_STATE_CONNECTING,   /**< Trying the stored network at boot. */
    PROVISIONING_STATE_CONNECTED,    /**< Station associated with the configured network. */
    PROVISIONING_STATE_DISCONNECTED, /**< Connection lost; reconnecting with backoff. */
    PROVISIONING_STATE_PORTAL,       /**< Provisioning mode: open AP and captive portal active. */
} provisioning_state_t;

/** @brief Connection-state-changed notification. Runs in the orchestrator task; it must not block. */
typedef void (*provisioning_state_cb_t)(provisioning_state_t state);

/**
 * @brief Register the connection-state-changed callback.
 *
 * Call before ProvisioningStart() to also receive the first state.
 *
 * @param callback Function to call on every state change, or NULL to unregister.
 */
void ProvisioningSetStateCallback(provisioning_state_cb_t callback);

/** @brief Initialize all components and start the orchestrator task. Call once from app_main(). */
void ProvisioningStart(void);

#ifdef __cplusplus
}
#endif
