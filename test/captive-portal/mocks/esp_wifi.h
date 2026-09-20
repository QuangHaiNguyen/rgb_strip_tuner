#pragma once
/* Host mock of esp_wifi.h: only the types and calls used by wifi_manager.c. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef enum { WIFI_STORAGE_FLASH = 0, WIFI_STORAGE_RAM } wifi_storage_t;
typedef enum { WIFI_SCAN_TYPE_ACTIVE = 0, WIFI_SCAN_TYPE_PASSIVE } wifi_scan_type_t;
typedef enum {
    WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA3_PSK,
    WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_ENT_192, WIFI_AUTH_WPA3_ENTERPRISE,
    WIFI_AUTH_WPA2_WPA3_ENTERPRISE, WIFI_AUTH_WPA_ENTERPRISE, WIFI_AUTH_OWE, WIFI_AUTH_WAPI_PSK
} wifi_auth_mode_t;
enum {
    WIFI_REASON_AUTH_EXPIRE = 2, WIFI_REASON_AUTH_LEAVE = 3, WIFI_REASON_ASSOC_LEAVE = 8,
    WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15, WIFI_REASON_BEACON_TIMEOUT = 200, WIFI_REASON_NO_AP_FOUND = 201,
    WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_ASSOC_FAIL = 203, WIFI_REASON_HANDSHAKE_TIMEOUT = 204
};
enum { WIFI_EVENT_STA_CONNECTED = 4, WIFI_EVENT_STA_DISCONNECTED = 5 };

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct { wifi_auth_mode_t authmode; } threshold;
    struct { bool capable; bool required; } pmf_cfg;
} wifi_sta_config_t;
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    uint8_t ssid_len;
    uint8_t channel;
    wifi_auth_mode_t authmode;
    uint8_t max_connection;
} wifi_ap_config_t;
typedef union { wifi_ap_config_t ap; wifi_sta_config_t sta; } wifi_config_t;
typedef struct {
    bool show_hidden;
    wifi_scan_type_t scan_type;
    struct { struct { uint32_t min; uint32_t max; } active; uint32_t passive; } scan_time;
} wifi_scan_config_t;
typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;
typedef struct { uint8_t reason; } wifi_event_sta_disconnected_t;
typedef struct { int nvs_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { .nvs_enable = 1 }

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *records);
#ifdef __cplusplus
}
#endif
