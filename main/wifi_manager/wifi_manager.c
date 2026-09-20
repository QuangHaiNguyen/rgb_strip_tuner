/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file wifi_manager.c
 * @brief Wi-Fi driver control shared by boot connection, provisioning and reconnect.
 */
#include "wifi_manager.h"
#include "logging.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIFI_AP_SSID_PREFIX "RGB-LED-Tuner-"
#define WIFI_AP_CHANNEL (1)
#define WIFI_SCAN_RAW_MAX (40)
#define WIFI_SCAN_CHANNEL_MIN_MS (100)
#define WIFI_SCAN_CHANNEL_MAX_MS (300)

LOG_MODULE_REGISTER("wifi", LOG_LEVEL_DEBUG);

static StaticSemaphore_t s_mutex_struct;
static SemaphoreHandle_t s_mutex;
static esp_netif_t *s_ap_netif;
static wifi_manager_event_cb_t s_on_event;
static bool s_is_started;
static wifi_ap_record_t s_records[WIFI_SCAN_RAW_MAX];
static wifi_scan_entry_t s_raw_entries[WIFI_SCAN_RAW_MAX];

uint32_t GetWifiReconnectDelayMs(uint32_t failure_count)
{
    uint32_t delay_ms = WIFI_RECONNECT_DELAY_MIN_MS;
    for (uint32_t index = 0; index < failure_count && delay_ms < WIFI_RECONNECT_DELAY_MAX_MS; ++index) {
        delay_ms *= 2;
    }
    return delay_ms > WIFI_RECONNECT_DELAY_MAX_MS ? WIFI_RECONNECT_DELAY_MAX_MS : delay_ms;
}

uint16_t NormalizeWifiScanEntries(wifi_scan_entry_t *entries, uint16_t count, uint16_t max_entries)
{
    for (uint16_t index = 1; index < count; ++index) {
        wifi_scan_entry_t current = entries[index];
        uint16_t position = index;
        while (position > 0 && entries[position - 1].rssi_dbm < current.rssi_dbm) {
            entries[position] = entries[position - 1];
            --position;
        }
        entries[position] = current;
    }

    uint16_t kept = 0;
    for (uint16_t index = 0; index < count && kept < max_entries; ++index) {
        if (entries[index].ssid[0] == '\0') {
            continue;
        }
        bool is_duplicate = false;
        for (uint16_t seen = 0; seen < kept && !is_duplicate; ++seen) {
            is_duplicate = (strcmp(entries[seen].ssid, entries[index].ssid) == 0);
        }
        if (!is_duplicate) {
            entries[kept++] = entries[index];
        }
    }
    return kept;
}

static const char *GetDisconnectReasonText(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
    case WIFI_REASON_AUTH_LEAVE: return "auth leave";
    case WIFI_REASON_ASSOC_LEAVE: return "assoc leave";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND: return "no ap found";
    case WIFI_REASON_AUTH_FAIL: return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
    default: return "other";
    }
}

static void HandleWifiEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_CONNECTED) {
        LOG_INFO("station connected");
        if (s_on_event != NULL) {
            s_on_event(WIFI_MANAGER_EVENT_STA_CONNECTED);
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)data;
        LOG_WARNING("station disconnected (%s)", event != NULL ? GetDisconnectReasonText(event->reason) : "unknown");
        if (s_on_event != NULL) {
            s_on_event(WIFI_MANAGER_EVENT_STA_DISCONNECTED);
        }
    }
}

/** @brief Make the AP's DHCP server offer the AP address as DNS server. Must run before the AP starts. */
static bool OfferAccessPointDns(void)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns_info = {0};
    uint8_t offer_dns = OFFER_DNS;

    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return false;
    }
    dns_info.ip.u_addr.ip4.addr = ip_info.ip.addr;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    return esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                  &offer_dns, sizeof(offer_dns)) == ESP_OK &&
           esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK;
}

bool InitWifiManager(wifi_manager_event_cb_t on_event)
{
    s_on_event = on_event;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_struct);

    (void)esp_netif_init();
    (void)esp_event_loop_create_default();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    (void)esp_netif_create_default_wifi_sta();
    if (!OfferAccessPointDns()) {
        LOG_WARNING("AP DHCP server will not offer DNS, the portal may not be detected automatically");
    }

    /* The driver must not persist settings: credentials live only in the encrypted store. */
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    init_config.nvs_enable = 0;
    if (esp_wifi_init(&init_config) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        LOG_ERROR("Wi-Fi driver init failed");
        return false;
    }
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, HandleWifiEvent, NULL, NULL);
    if (err != ESP_OK) {
        LOG_ERROR("Wi-Fi event handler registration failed: %d", err);
        return false;
    }
    LOG_INFO("Wi-Fi manager ready");
    return true;
}

bool StartWifiAccessPoint(void)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);

    wifi_config_t config = {0};
    int ssid_length = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    config.ap.ssid_len = (uint8_t)ssid_length;
    config.ap.channel = WIFI_AP_CHANNEL;
    config.ap.max_connection = WIFI_AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_OPEN;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool is_ok = esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
                 esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK &&
                 (s_is_started || esp_wifi_start() == ESP_OK);
    s_is_started = s_is_started || is_ok;
    xSemaphoreGive(s_mutex);

    if (is_ok) {
        LOG_INFO("open access point started: %s", (const char *)config.ap.ssid);
    } else {
        LOG_ERROR("failed to start access point");
    }
    return is_ok;
}

bool StopWifiAccessPoint(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool is_ok = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
    xSemaphoreGive(s_mutex);
    LOG_INFO("access point stopped");
    return is_ok;
}

bool ConnectWifiStation(const wifi_credentials_t *credentials)
{
    if (!IsCredentialsValid(credentials)) {
        return false;
    }

    wifi_config_t config = {0};
    size_t password_length = strlen(credentials->password);
    memcpy(config.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(config.sta.password, credentials->password, password_length);
    config.sta.threshold.authmode = (password_length == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    bool is_ok = (mode == WIFI_MODE_APSTA || esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) &&
                 esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK &&
                 (s_is_started || esp_wifi_start() == ESP_OK) &&
                 esp_wifi_connect() == ESP_OK;
    s_is_started = s_is_started || is_ok;
    xSemaphoreGive(s_mutex);

    memset(&config, 0, sizeof(config));
    LOG_DEBUG("station connect %s for %s", is_ok ? "started" : "failed to start", credentials->ssid);
    return is_ok;
}

void DisconnectWifiStation(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_is_started) {
        (void)esp_wifi_disconnect();
    }
    xSemaphoreGive(s_mutex);
}

/** @brief Networks ConnectWifiStation() can join: open and the WPA2/WPA3 personal modes. */
static bool IsSupportedAuth(wifi_auth_mode_t authmode)
{
    return authmode == WIFI_AUTH_OPEN || authmode == WIFI_AUTH_WPA2_PSK || authmode == WIFI_AUTH_WPA_WPA2_PSK ||
           authmode == WIFI_AUTH_WPA3_PSK || authmode == WIFI_AUTH_WPA2_WPA3_PSK;
}

int ScanWifiNetworks(wifi_scan_entry_t *entries, uint16_t max_entries)
{
    wifi_scan_config_t scan_config = {0};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = WIFI_SCAN_CHANNEL_MIN_MS;
    scan_config.scan_time.active.max = WIFI_SCAN_CHANNEL_MAX_MS;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t record_count = WIFI_SCAN_RAW_MAX;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&record_count, s_records);
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        LOG_WARNING("Wi-Fi scan failed: %d", err);
        return -1;
    }

    for (uint16_t index = 0; index < record_count; ++index) {
        wifi_scan_entry_t *raw = &s_raw_entries[index];
        memset(raw, 0, sizeof(*raw));
        strncpy(raw->ssid, (const char *)s_records[index].ssid, CREDENTIAL_SSID_MAX);
        raw->rssi_dbm = s_records[index].rssi;
        raw->is_open = (s_records[index].authmode == WIFI_AUTH_OPEN);
        raw->is_unsupported = !IsSupportedAuth(s_records[index].authmode);
    }
    uint16_t limit = max_entries < WIFI_SCAN_MAX_ENTRIES ? max_entries : WIFI_SCAN_MAX_ENTRIES;
    uint16_t kept = NormalizeWifiScanEntries(s_raw_entries, record_count, limit);
    memcpy(entries, s_raw_entries, kept * sizeof(entries[0]));
    xSemaphoreGive(s_mutex);

    LOG_INFO("Wi-Fi scan complete: %u networks", (unsigned)kept);
    return (int)kept;
}

uint32_t GetWifiAccessPointAddress(void)
{
    esp_netif_ip_info_t ip_info = {0};
    if (s_ap_netif == NULL || esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        return 0;
    }
    return ip_info.ip.addr;
}
