#include "fff.h"
DEFINE_FFF_GLOBALS;

#include <string.h>
#include "wifi_fakes.h"

esp_event_base_t WIFI_EVENT = "WIFI_EVENT";

FAKE_VALUE_FUNC(esp_err_t, esp_wifi_init, const wifi_init_config_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_set_storage, wifi_storage_t);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_set_mode, wifi_mode_t);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_get_mode, wifi_mode_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_set_config, wifi_interface_t, wifi_config_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_start);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_connect);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_disconnect);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_scan_start, const wifi_scan_config_t *, bool);
FAKE_VALUE_FUNC(esp_err_t, esp_wifi_scan_get_ap_records, uint16_t *, wifi_ap_record_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_event_loop_create_default);
FAKE_VALUE_FUNC(esp_err_t, esp_event_handler_instance_register, esp_event_base_t, int32_t, esp_event_handler_t,
                void *, esp_event_handler_instance_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_netif_init);
FAKE_VALUE_FUNC(esp_netif_t *, esp_netif_create_default_wifi_ap);
FAKE_VALUE_FUNC(esp_netif_t *, esp_netif_create_default_wifi_sta);
FAKE_VALUE_FUNC(esp_err_t, esp_netif_get_ip_info, esp_netif_t *, esp_netif_ip_info_t *);
FAKE_VALUE_FUNC(esp_err_t, esp_read_mac, uint8_t *, esp_mac_type_t);
FAKE_VALUE_FUNC(esp_err_t, esp_netif_dhcps_option, esp_netif_t *, esp_netif_dhcp_option_mode_t,
                esp_netif_dhcp_option_id_t, void *, uint32_t);
FAKE_VALUE_FUNC(esp_err_t, esp_netif_set_dns_info, esp_netif_t *, esp_netif_dns_type_t, esp_netif_dns_info_t *);

static wifi_mode_t s_mode;
static wifi_config_t s_ap_config, s_sta_config;
static wifi_init_config_t s_init_config;
static wifi_scan_config_t s_scan_config;
static int s_scan_blocking;
static wifi_ap_record_t s_records[64];
static uint16_t s_record_count;
static uint8_t s_mac[6];
static uint32_t s_ap_addr;
static esp_event_handler_t s_handler;
static int s_last_mac_type;
static char s_fail_call[40];
static int s_dhcps_mode, s_dhcps_id, s_dhcps_value;
static uint32_t s_dns_address;
static int s_order_dhcps, s_order_dns, s_order_start;
static esp_netif_t *const s_netif_marker = (esp_netif_t *)0x1000;

static bool ShouldFail(const char *name) { return strcmp(s_fail_call, name) == 0; }

static esp_err_t InitFake(const wifi_init_config_t *config)
{
    s_init_config = *config;
    return ShouldFail("esp_wifi_init") ? ESP_FAIL : ESP_OK;
}
static esp_err_t SetStorageFake(wifi_storage_t s) { (void)s; return ShouldFail("esp_wifi_set_storage") ? ESP_FAIL : ESP_OK; }
static esp_err_t SetModeFake(wifi_mode_t mode)
{
    if (ShouldFail("esp_wifi_set_mode")) return ESP_FAIL;
    s_mode = mode;
    return ESP_OK;
}
static esp_err_t GetModeFake(wifi_mode_t *mode) { *mode = s_mode; return ESP_OK; }
static esp_err_t SetConfigFake(wifi_interface_t interface, wifi_config_t *config)
{
    if (ShouldFail("esp_wifi_set_config")) return ESP_FAIL;
    memcpy(interface == WIFI_IF_AP ? &s_ap_config : &s_sta_config, config, sizeof(*config));
    return ESP_OK;
}
static esp_err_t StartFake(void)
{
    if (s_order_start == 0) s_order_start = (int)fff.call_history_idx + 1;
    return ShouldFail("esp_wifi_start") ? ESP_FAIL : ESP_OK;
}
static esp_err_t DhcpsOptionFake(esp_netif_t *netif, esp_netif_dhcp_option_mode_t mode, esp_netif_dhcp_option_id_t id,
                                 void *value, uint32_t length)
{
    (void)netif; (void)length;
    s_dhcps_mode = (int)mode;
    s_dhcps_id = (int)id;
    s_dhcps_value = *(uint8_t *)value;
    if (s_order_dhcps == 0) s_order_dhcps = (int)fff.call_history_idx + 1;
    return ShouldFail("esp_netif_dhcps_option") ? ESP_FAIL : ESP_OK;
}
static esp_err_t SetDnsInfoFake(esp_netif_t *netif, esp_netif_dns_type_t type, esp_netif_dns_info_t *dns)
{
    (void)netif; (void)type;
    s_dns_address = dns->ip.u_addr.ip4.addr;
    if (s_order_dns == 0) s_order_dns = (int)fff.call_history_idx + 1;
    return ShouldFail("esp_netif_set_dns_info") ? ESP_FAIL : ESP_OK;
}
static esp_err_t ConnectFake(void) { return ShouldFail("esp_wifi_connect") ? ESP_FAIL : ESP_OK; }
static esp_err_t ScanStartFake(const wifi_scan_config_t *config, bool block)
{
    s_scan_config = *config;
    s_scan_blocking = block;
    return ShouldFail("esp_wifi_scan_start") ? ESP_ERR_WIFI_STATE : ESP_OK;
}
static esp_err_t ScanGetFake(uint16_t *number, wifi_ap_record_t *records)
{
    if (ShouldFail("esp_wifi_scan_get_ap_records")) return ESP_FAIL;
    uint16_t count = s_record_count < *number ? s_record_count : *number;
    memcpy(records, s_records, count * sizeof(records[0]));
    *number = count;
    return ESP_OK;
}
static esp_err_t RegisterFake(esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg,
                              esp_event_handler_instance_t *instance)
{
    (void)base; (void)id; (void)arg; (void)instance;
    s_handler = handler;
    return ShouldFail("esp_event_handler_instance_register") ? ESP_FAIL : ESP_OK;
}
static esp_netif_t *CreateNetifFake(void) { return s_netif_marker; }
static esp_err_t GetIpInfoFake(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    (void)netif;
    memset(info, 0, sizeof(*info));
    info->ip.addr = s_ap_addr;
    return ESP_OK;
}
static esp_err_t ReadMacFake(uint8_t *mac, esp_mac_type_t type)
{
    memcpy(mac, s_mac, 6);
    s_last_mac_type = (int)type;
    return ESP_OK;
}

void TestWifiReset(void)
{
    RESET_FAKE(esp_wifi_init); RESET_FAKE(esp_wifi_set_storage); RESET_FAKE(esp_wifi_set_mode);
    RESET_FAKE(esp_wifi_get_mode); RESET_FAKE(esp_wifi_set_config); RESET_FAKE(esp_wifi_start);
    RESET_FAKE(esp_wifi_connect); RESET_FAKE(esp_wifi_disconnect); RESET_FAKE(esp_wifi_scan_start);
    RESET_FAKE(esp_wifi_scan_get_ap_records); RESET_FAKE(esp_event_loop_create_default);
    RESET_FAKE(esp_event_handler_instance_register); RESET_FAKE(esp_netif_init);
    RESET_FAKE(esp_netif_create_default_wifi_ap); RESET_FAKE(esp_netif_create_default_wifi_sta);
    RESET_FAKE(esp_netif_get_ip_info); RESET_FAKE(esp_read_mac);
    RESET_FAKE(esp_netif_dhcps_option); RESET_FAKE(esp_netif_set_dns_info);
    FFF_RESET_HISTORY();
    esp_wifi_init_fake.custom_fake = InitFake;
    esp_wifi_set_storage_fake.custom_fake = SetStorageFake;
    esp_wifi_set_mode_fake.custom_fake = SetModeFake;
    esp_wifi_get_mode_fake.custom_fake = GetModeFake;
    esp_wifi_set_config_fake.custom_fake = SetConfigFake;
    esp_wifi_start_fake.custom_fake = StartFake;
    esp_wifi_connect_fake.custom_fake = ConnectFake;
    esp_wifi_scan_start_fake.custom_fake = ScanStartFake;
    esp_wifi_scan_get_ap_records_fake.custom_fake = ScanGetFake;
    esp_event_handler_instance_register_fake.custom_fake = RegisterFake;
    esp_netif_create_default_wifi_ap_fake.custom_fake = CreateNetifFake;
    esp_netif_create_default_wifi_sta_fake.custom_fake = CreateNetifFake;
    esp_netif_get_ip_info_fake.custom_fake = GetIpInfoFake;
    esp_read_mac_fake.custom_fake = ReadMacFake;
    esp_netif_dhcps_option_fake.custom_fake = DhcpsOptionFake;
    esp_netif_set_dns_info_fake.custom_fake = SetDnsInfoFake;
    s_dhcps_mode = s_dhcps_id = s_dhcps_value = 0;
    s_dns_address = 0;
    s_order_dhcps = s_order_dns = s_order_start = 0;
    s_mode = WIFI_MODE_NULL;
    memset(&s_ap_config, 0, sizeof(s_ap_config));
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    memset(&s_init_config, 0, sizeof(s_init_config));
    memset(&s_scan_config, 0, sizeof(s_scan_config));
    memset(s_mac, 0, sizeof(s_mac));
    s_record_count = 0;
    s_ap_addr = 0;
    s_handler = NULL;
    s_scan_blocking = -1;
    s_fail_call[0] = '\0';
}

void TestWifiSetMac(const uint8_t mac[6]) { memcpy(s_mac, mac, 6); }
void TestWifiSetApAddress(uint32_t addr) { s_ap_addr = addr; }
void TestWifiSetScanRecords(const wifi_ap_record_t *records, uint16_t count)
{
    memcpy(s_records, records, count * sizeof(records[0]));
    s_record_count = count;
}
void TestWifiFailCall(const char *name) { strncpy(s_fail_call, name, sizeof(s_fail_call) - 1); }
void TestWifiFireEvent(int32_t id, const wifi_event_sta_disconnected_t *data)
{
    if (s_handler != NULL) {
        s_handler(NULL, WIFI_EVENT, id, (void *)data);
    }
}
int TestWifiHandlerRegistered(void) { return s_handler != NULL; }
wifi_mode_t TestWifiCurrentMode(void) { return s_mode; }
const wifi_config_t *TestWifiLastApConfig(void) { return &s_ap_config; }
const wifi_config_t *TestWifiLastStaConfig(void) { return &s_sta_config; }
const wifi_init_config_t *TestWifiLastInitConfig(void) { return &s_init_config; }
const wifi_scan_config_t *TestWifiLastScanConfig(void) { return &s_scan_config; }
int TestWifiLastScanBlocking(void) { return s_scan_blocking; }
int TestWifiLastMacType(void) { return s_last_mac_type; }

int TestWifiDhcpsOptionCalls(void) { return (int)esp_netif_dhcps_option_fake.call_count; }
int TestWifiDhcpsOptionId(void) { return s_dhcps_id; }
int TestWifiDhcpsOptionMode(void) { return s_dhcps_mode; }
int TestWifiDhcpsOptionValue(void) { return s_dhcps_value; }
int TestWifiDnsInfoCalls(void) { return (int)esp_netif_set_dns_info_fake.call_count; }
uint32_t TestWifiDnsInfoAddress(void) { return s_dns_address; }
int TestWifiCallOrder(const char *name)
{
    if (strcmp(name, "esp_netif_dhcps_option") == 0) return s_order_dhcps;
    if (strcmp(name, "esp_netif_set_dns_info") == 0) return s_order_dns;
    if (strcmp(name, "esp_wifi_start") == 0) return s_order_start;
    return -1;
}

int TestWifiCalls(const char *name)
{
#define CALLS(fake, label) if (strcmp(name, label) == 0) return (int)fake.call_count
    CALLS(esp_wifi_init_fake, "esp_wifi_init");
    CALLS(esp_wifi_set_storage_fake, "esp_wifi_set_storage");
    CALLS(esp_wifi_set_mode_fake, "esp_wifi_set_mode");
    CALLS(esp_wifi_set_config_fake, "esp_wifi_set_config");
    CALLS(esp_wifi_start_fake, "esp_wifi_start");
    CALLS(esp_wifi_connect_fake, "esp_wifi_connect");
    CALLS(esp_wifi_disconnect_fake, "esp_wifi_disconnect");
    CALLS(esp_wifi_scan_start_fake, "esp_wifi_scan_start");
    CALLS(esp_event_handler_instance_register_fake, "esp_event_handler_instance_register");
#undef CALLS
    return -1;
}
