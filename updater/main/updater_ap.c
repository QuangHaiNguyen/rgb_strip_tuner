/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_ap.c
 * @brief Open SoftAP of the updater (SPEC-007 FR-22, FR-33). No DNS server (section 11, row 6).
 */
#include "updater_ap.h"
#include <stdio.h>
#include <string.h>
#include "logging.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#define AP_SSID_PREFIX "RGB-LED-Updater-"
#define AP_LEASE_COUNT (10)

LOG_MODULE_REGISTER("updater", LOG_LEVEL_DEBUG);

/** @brief Give the SoftAP interface 192.168.0.1/24 and lease addresses from 192.168.0.2. */
static bool ConfigureApAddress(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 0, 1);
    esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 0, 1);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);

    dhcps_lease_t lease = {.enable = true};
    lease.start_ip.addr = ESP_IP4TOADDR(192, 168, 0, 2);
    lease.end_ip.addr = ESP_IP4TOADDR(192, 168, 0, 2 + AP_LEASE_COUNT - 1);

    esp_err_t err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return false;
    }
    return esp_netif_set_ip_info(netif, &ip_info) == ESP_OK &&
           esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)) ==
               ESP_OK &&
           esp_netif_dhcps_start(netif) == ESP_OK;
}

bool StartUpdaterAp(void)
{
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        LOG_ERROR("network stack init failed");
        return false;
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL || !ConfigureApAddress(netif)) {
        LOG_ERROR("SoftAP address setup failed");
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_config) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        LOG_ERROR("Wi-Fi init failed");
        return false;
    }

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t config = {
        .ap = {
            .channel = UPDATER_AP_CHANNEL,
            .max_connection = UPDATER_AP_MAX_CLIENTS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    int ssid_len = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
    config.ap.ssid_len = (uint8_t)ssid_len;

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        LOG_ERROR("SoftAP start failed");
        return false;
    }
    LOG_INFO("AP %s at 192.168.0.1", (const char *)config.ap.ssid);
    return true;
}
