/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file wifi_manager.h
 * @brief Wi-Fi station/SoftAP control, scanning and reconnect backoff (SPEC-002 FR-7, FR-8, FR-11, FR-12, FR-15, FR-21).
 *
 * All calls that touch the Wi-Fi driver are serialized by an internal mutex.
 * Connection attempts are non-blocking; results are reported through the event
 * callback so the caller can run its own timers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "credential_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of networks returned by ScanWifiNetworks(). */
#define WIFI_SCAN_MAX_ENTRIES (20)
/** @brief Maximum number of simultaneous SoftAP clients. */
#define WIFI_AP_MAX_CLIENTS (4)
/** @brief Upper bound of a scan, in milliseconds. */
#define WIFI_SCAN_TIMEOUT_MS (10000)
/** @brief First reconnect delay in milliseconds. */
#define WIFI_RECONNECT_DELAY_MIN_MS (1000)
/** @brief Reconnect delay cap in milliseconds. */
#define WIFI_RECONNECT_DELAY_MAX_MS (60000)

/** @brief One scanned network. */
typedef struct {
    char ssid[CREDENTIAL_SSID_MAX + 1]; /**< Null-terminated SSID. */
    int8_t rssi_dbm;                    /**< Signal strength in dBm. */
    bool is_open;                       /**< No authentication required. */
    bool is_unsupported;                /**< Cannot be joined: enterprise, WEP, WPA1-only, OWE or other. */
} wifi_scan_entry_t;

/** @brief Station events reported to the owner. */
typedef enum {
    WIFI_MANAGER_EVENT_STA_CONNECTED,    /**< Station associated with the access point. */
    WIFI_MANAGER_EVENT_STA_DISCONNECTED, /**< Station association lost or attempt failed. */
} wifi_manager_event_t;

/** @brief Event callback. Runs in the system event task; it must not block. */
typedef void (*wifi_manager_event_cb_t)(wifi_manager_event_t event);

/**
 * @brief Compute the reconnect delay after a number of failed reconnect attempts (pure function).
 *
 * 1 s for the first retry, doubling each time, capped at 60 s.
 *
 * @param failure_count Number of failed reconnect attempts so far (0 for the first retry).
 * @return Delay in milliseconds.
 */
uint32_t GetWifiReconnectDelayMs(uint32_t failure_count);

/**
 * @brief Sort, deduplicate and truncate raw scan results (pure function).
 *
 * Entries are ordered strongest first, each SSID appears once (strongest kept),
 * empty SSIDs are dropped, and at most @p max_entries are kept.
 *
 * @param[in,out] entries     Raw entries; reordered in place.
 * @param[in]     count       Number of raw entries.
 * @param[in]     max_entries Maximum entries to keep.
 * @return Number of entries kept, stored at the start of @p entries.
 */
uint16_t NormalizeWifiScanEntries(wifi_scan_entry_t *entries, uint16_t count, uint16_t max_entries);

/**
 * @brief Initialize the Wi-Fi driver, network interfaces and event handling.
 *
 * Wi-Fi settings are kept in RAM only, so credentials are never written to the driver's NVS.
 * The AP's DHCP server is set to hand out the AP address as DNS server, so clients query the
 * wildcard DNS server and detect the captive portal (FR-9, FR-10).
 *
 * @param on_event Station event callback.
 * @return true on success.
 */
bool InitWifiManager(wifi_manager_event_cb_t on_event);

/**
 * @brief Start the open SoftAP (keeping the station interface) and begin serving DHCP.
 *
 * The SSID is `RGB-LED-Tuner-XXXX` with the last four uppercase hex digits of the base MAC.
 *
 * @return true on success.
 */
bool StartWifiAccessPoint(void);

/**
 * @brief Stop the SoftAP and continue in station-only mode without dropping the station link.
 *
 * @return true on success.
 */
bool StopWifiAccessPoint(void);

/**
 * @brief Start one non-blocking station connection attempt.
 *
 * Works in station-only and AP+station mode. An empty password selects open authentication.
 *
 * @param credentials Network to join.
 * @return true if the attempt was started.
 */
bool ConnectWifiStation(const wifi_credentials_t *credentials);

/** @brief Abort any connection attempt and drop the station link. */
void DisconnectWifiStation(void);

/**
 * @brief Scan for networks, at most WIFI_SCAN_TIMEOUT_MS.
 *
 * @param[out] entries     Result array.
 * @param[in]  max_entries Capacity of @p entries; at most WIFI_SCAN_MAX_ENTRIES are returned.
 * @return Number of entries, or -1 if the scan could not run (e.g. a connection attempt is in progress).
 */
int ScanWifiNetworks(wifi_scan_entry_t *entries, uint16_t max_entries);

/**
 * @brief Get the SoftAP IPv4 address.
 *
 * @return Address in network byte order, or 0 if unavailable.
 */
uint32_t GetWifiAccessPointAddress(void);

#ifdef __cplusplus
}
#endif
