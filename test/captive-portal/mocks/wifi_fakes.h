#pragma once
/** @file wifi_fakes.h @brief FFF fakes for the ESP-IDF Wi-Fi, netif and event APIs used by wifi_manager.c. */
#include <stdint.h>
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#ifdef __cplusplus
extern "C" {
#endif
void TestWifiReset(void);
void TestWifiSetMac(const uint8_t mac[6]);
void TestWifiSetApAddress(uint32_t addr);
void TestWifiSetScanRecords(const wifi_ap_record_t *records, uint16_t count);
/** Make the Nth-named call fail, e.g. "esp_wifi_start". */
void TestWifiFailCall(const char *name);
/** Deliver a Wi-Fi event to the handler registered by wifi_manager.c. */
void TestWifiFireEvent(int32_t id, const wifi_event_sta_disconnected_t *data);
int TestWifiHandlerRegistered(void);
wifi_mode_t TestWifiCurrentMode(void);
const wifi_config_t *TestWifiLastApConfig(void);
const wifi_config_t *TestWifiLastStaConfig(void);
const wifi_init_config_t *TestWifiLastInitConfig(void);
const wifi_scan_config_t *TestWifiLastScanConfig(void);
int TestWifiLastScanBlocking(void);
int TestWifiCalls(const char *name);
int TestWifiLastMacType(void);
/** Last esp_netif_dhcps_option() / esp_netif_set_dns_info() arguments. */
int TestWifiDhcpsOptionCalls(void);
int TestWifiDhcpsOptionId(void);
int TestWifiDhcpsOptionMode(void);
int TestWifiDhcpsOptionValue(void);
int TestWifiDnsInfoCalls(void);
uint32_t TestWifiDnsInfoAddress(void);
/** Position of the first esp_netif_dhcps_option()/esp_netif_set_dns_info()/esp_wifi_start() call in the overall call order. */
int TestWifiCallOrder(const char *name);
#ifdef __cplusplus
}
#endif
