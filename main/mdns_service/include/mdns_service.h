/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file mdns_service.h
 * @brief mDNS responder for station mode: `rgb-tuner.local` and one `_http._tcp` service (SPEC-005 FR-17..FR-21).
 *
 * The only project component that includes the managed component's `mdns.h` (SPEC-005 NFR-10).
 * The responder is bound to the default Wi-Fi station interface through the component's
 * predefined station netif, which also follows got-IP and disconnect events on its own (FR-20, FR-22).
 *
 * Single caller: StartMdnsService(), StopMdnsService() and LogMdnsHostnameInUse() are called only
 * from the provisioning orchestrator task, so they take no lock (SPEC-005 NFR-9).
 * ClassifyMdnsHostname() is pure and may be called from anywhere.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief mDNS host name claimed by the device, without `.local`. */
#define MDNS_SERVICE_HOSTNAME "rgb-tuner"
/** @brief DNS-SD instance name of the device and of its HTTP service. */
#define MDNS_SERVICE_INSTANCE "RGB LED Tuner"
/** @brief TCP port advertised for the `_http._tcp` service. */
#define MDNS_SERVICE_PORT (80)
/** @brief Buffer size for a host name without `.local`: 63 characters (DNS label limit) plus terminator. */
#define MDNS_SERVICE_HOSTNAME_MAX (64)

/** @brief Classification of the host name the responder actually uses. */
typedef enum {
    MDNS_HOSTNAME_DEFAULT,     /**< The name is exactly MDNS_SERVICE_HOSTNAME. */
    MDNS_HOSTNAME_RENAMED,     /**< Conflict probing renamed the host, e.g. `rgb-tuner-2`. */
    MDNS_HOSTNAME_UNAVAILABLE, /**< No name: NULL or empty. */
} mdns_hostname_status_t;

/**
 * @brief Start the mDNS responder and advertise the tuner web page (FR-18).
 *
 * Sets the host name MDNS_SERVICE_HOSTNAME, the default instance MDNS_SERVICE_INSTANCE and adds the
 * service `MDNS_SERVICE_INSTANCE._http._tcp` on MDNS_SERVICE_PORT with the single TXT item `path=/`.
 * Idempotent: returns true at once when already running. On any failing step the responder is
 * freed again and an Error is logged.
 *
 * @return true if the responder runs.
 */
bool StartMdnsService(void);

/** @brief Stop the responder and free its resources (FR-19). Does nothing when it is not running. */
void StopMdnsService(void);

/**
 * @brief Read, log and return the host name in use (FR-21).
 *
 * Logs Info `mDNS hostname in use: rgb-tuner.local` for the default name, Warning
 * `mDNS hostname conflict: using <name>.local instead of rgb-tuner.local` after a rename, and
 * Warning `mDNS hostname unavailable (err=<code>)` when the name cannot be read.
 *
 * @param[out] hostname_in_use Receives the name without `.local`; unchanged on failure.
 * @param[in]  hostname_size   Size of @p hostname_in_use, at least MDNS_SERVICE_HOSTNAME_MAX.
 * @return true if a name was read and copied.
 */
bool LogMdnsHostnameInUse(char *hostname_in_use, size_t hostname_size);

/**
 * @brief Classify a host name against MDNS_SERVICE_HOSTNAME (pure function, no ESP-IDF dependency).
 *
 * @param hostname_in_use Name without `.local`, or NULL.
 * @return MDNS_HOSTNAME_DEFAULT, MDNS_HOSTNAME_RENAMED, or MDNS_HOSTNAME_UNAVAILABLE for NULL or empty.
 */
mdns_hostname_status_t ClassifyMdnsHostname(const char *hostname_in_use);

#ifdef __cplusplus
}
#endif
