/**
 * @file test_wifi_manager.cpp
 * @brief Host tests for the Wi-Fi manager (SPEC-002 T-3, T-5, T-10; FR-7, FR-8, FR-11, FR-12, FR-15, FR-20, FR-21, NFR-1).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "freertos_mock.h"
#include "host_stubs.h"
#include "wifi_fakes.h"
#include "dhcpserver/dhcpserver.h"
#include "wifi_manager.h"
void HarnessResetWifiManager(void);
}

namespace {

std::vector<wifi_manager_event_t> g_events;

void OnEvent(wifi_manager_event_t event)
{
    g_events.push_back(event);
}

void Reset()
{
    MockFreeRtosReset();
    TestWifiReset();
    TestLogReset();
    HarnessResetWifiManager();
    g_events.clear();
}

void InitManager()
{
    Reset();
    REQUIRE(InitWifiManager(OnEvent));
}

wifi_ap_record_t MakeRecord(const char *ssid, int rssi_dbm, wifi_auth_mode_t auth)
{
    wifi_ap_record_t record = {};
    std::strncpy(reinterpret_cast<char *>(record.ssid), ssid, sizeof(record.ssid) - 1);
    record.rssi = static_cast<int8_t>(rssi_dbm);
    record.authmode = auth;
    return record;
}

wifi_scan_entry_t MakeEntry(const char *ssid, int rssi_dbm, bool open = false, bool unsupported = false)
{
    wifi_scan_entry_t entry = {};
    std::strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    entry.rssi_dbm = static_cast<int8_t>(rssi_dbm);
    entry.is_open = open;
    entry.is_unsupported = unsupported;
    return entry;
}

/** Returns a pointer into a small ring, so calls can be written inline as arguments. */
const wifi_credentials_t *MakeCredentials(const char *ssid, const char *password)
{
    static wifi_credentials_t ring[4];
    static int next = 0;
    wifi_credentials_t &credentials = ring[next++ % 4];
    credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return &credentials;
}

std::string ToString(const uint8_t *bytes, size_t max_length)
{
    return std::string(reinterpret_cast<const char *>(bytes), strnlen(reinterpret_cast<const char *>(bytes), max_length));
}

}  // namespace

TEST_CASE("reconnect delay doubles from 1 s up to a 60 s cap", "[T-10][FR-21]")
{
    const uint32_t expected_ms[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    for (uint32_t failures = 0; failures < 9; ++failures) {
        INFO("failures " << failures);
        REQUIRE(GetWifiReconnectDelayMs(failures) == expected_ms[failures]);
    }
    REQUIRE(GetWifiReconnectDelayMs(1000) == 60000);
    REQUIRE(GetWifiReconnectDelayMs(UINT32_MAX) == 60000);
}

TEST_CASE("scan normalization sorts, deduplicates and truncates", "[T-5][FR-11]")
{
    SECTION("strongest first") {
        wifi_scan_entry_t entries[] = {MakeEntry("c", -80), MakeEntry("a", -30), MakeEntry("b", -55)};
        REQUIRE(NormalizeWifiScanEntries(entries, 3, 20) == 3);
        REQUIRE(std::string(entries[0].ssid) == "a");
        REQUIRE(std::string(entries[1].ssid) == "b");
        REQUIRE(std::string(entries[2].ssid) == "c");
    }
    SECTION("each SSID once, keeping the strongest") {
        wifi_scan_entry_t entries[] = {MakeEntry("home", -70), MakeEntry("other", -60), MakeEntry("home", -40),
                                       MakeEntry("home", -90)};
        REQUIRE(NormalizeWifiScanEntries(entries, 4, 20) == 2);
        REQUIRE(std::string(entries[0].ssid) == "home");
        REQUIRE(entries[0].rssi_dbm == -40);
        REQUIRE(std::string(entries[1].ssid) == "other");
    }
    SECTION("hidden (empty) SSIDs are dropped") {
        wifi_scan_entry_t entries[] = {MakeEntry("", -20), MakeEntry("x", -50)};
        REQUIRE(NormalizeWifiScanEntries(entries, 2, 20) == 1);
        REQUIRE(std::string(entries[0].ssid) == "x");
    }
    SECTION("at most max_entries, the strongest ones") {
        std::vector<wifi_scan_entry_t> entries;
        for (int index = 0; index < 30; ++index) {
            entries.push_back(MakeEntry(("net" + std::to_string(index)).c_str(), -90 + index));
        }
        REQUIRE(NormalizeWifiScanEntries(entries.data(), 30, 20) == 20);
        REQUIRE(std::string(entries[0].ssid) == "net29");
        REQUIRE(std::string(entries[19].ssid) == "net10");
    }
    SECTION("flags travel with the entry") {
        wifi_scan_entry_t entries[] = {MakeEntry("weak", -80, true, false), MakeEntry("corp", -40, false, true)};
        NormalizeWifiScanEntries(entries, 2, 20);
        REQUIRE(entries[0].is_unsupported);
        REQUIRE(entries[1].is_open);
    }
    SECTION("empty input") {
        REQUIRE(NormalizeWifiScanEntries(nullptr, 0, 20) == 0);
    }
}

TEST_CASE("init keeps Wi-Fi settings out of NVS", "[T-7][NFR-1]")
{
    Reset();
    REQUIRE(InitWifiManager(OnEvent));

    REQUIRE(TestWifiCalls("esp_wifi_init") == 1);
    REQUIRE(TestWifiLastInitConfig()->nvs_enable == 0);
    REQUIRE(TestWifiCalls("esp_wifi_set_storage") == 1);
    REQUIRE(TestWifiHandlerRegistered());
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("init failures are reported", "[T-2][NFR-8]")
{
    Reset();
    SECTION("driver init") {
        TestWifiFailCall("esp_wifi_init");
    }
    SECTION("storage mode") {
        TestWifiFailCall("esp_wifi_set_storage");
    }
    SECTION("event handler") {
        TestWifiFailCall("esp_event_handler_instance_register");
    }
    REQUIRE_FALSE(InitWifiManager(OnEvent));
    REQUIRE(TestLogCount(3) >= 1);
}

TEST_CASE("station events are forwarded to the owner", "[T-10][FR-21]")
{
    InitManager();

    TestWifiFireEvent(WIFI_EVENT_STA_CONNECTED, nullptr);
    REQUIRE(g_events.size() == 1);
    REQUIRE(g_events[0] == WIFI_MANAGER_EVENT_STA_CONNECTED);

    wifi_event_sta_disconnected_t lost = {WIFI_REASON_NO_AP_FOUND};
    TestWifiFireEvent(WIFI_EVENT_STA_DISCONNECTED, &lost);
    REQUIRE(g_events.size() == 2);
    REQUIRE(g_events[1] == WIFI_MANAGER_EVENT_STA_DISCONNECTED);
    REQUIRE(TestLogCount(2) == 1);                       // failures are logged at Warning

    TestWifiFireEvent(WIFI_EVENT_STA_DISCONNECTED, nullptr);  // no payload must not crash
    REQUIRE(g_events.size() == 3);

    TestWifiFireEvent(99, nullptr);                      // unrelated events are ignored
    REQUIRE(g_events.size() == 3);
}

TEST_CASE("the AP is open and named RGB-LED-Tuner plus the last four MAC digits", "[T-3][FR-7][FR-8]")
{
    InitManager();
    const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 0xAB, 0xcd, 0xef};
    TestWifiSetMac(mac);

    REQUIRE(StartWifiAccessPoint());

    const wifi_ap_config_t &ap = TestWifiLastApConfig()->ap;
    REQUIRE(ToString(ap.ssid, sizeof(ap.ssid)) == "RGB-LED-Tuner-CDEF");   // uppercase
    REQUIRE(ap.ssid_len == std::strlen("RGB-LED-Tuner-CDEF"));
    REQUIRE(ap.authmode == WIFI_AUTH_OPEN);
    REQUIRE(ToString(ap.password, sizeof(ap.password)).empty());
    REQUIRE(ap.max_connection == 4);
    REQUIRE(TestWifiCurrentMode() == WIFI_MODE_APSTA);
    REQUIRE(TestWifiCalls("esp_wifi_start") == 1);
    REQUIRE(TestWifiLastMacType() == ESP_MAC_BASE);
}

TEST_CASE("SSID suffix is zero padded", "[T-3][FR-8]")
{
    InitManager();
    const uint8_t mac[6] = {0, 0, 0, 0, 0x01, 0x0A};
    TestWifiSetMac(mac);
    REQUIRE(StartWifiAccessPoint());
    REQUIRE(ToString(TestWifiLastApConfig()->ap.ssid, 32) == "RGB-LED-Tuner-010A");
}

TEST_CASE("starting the AP twice starts the driver once", "[T-8][FR-20]")
{
    InitManager();
    REQUIRE(StartWifiAccessPoint());
    REQUIRE(StartWifiAccessPoint());
    REQUIRE(TestWifiCalls("esp_wifi_start") == 1);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("an AP start failure is reported", "[T-3][FR-7]")
{
    InitManager();
    TestWifiFailCall("esp_wifi_start");
    REQUIRE_FALSE(StartWifiAccessPoint());
    REQUIRE(TestLogCount(3) >= 1);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("stopping the AP keeps the station link", "[T-7][FR-18]")
{
    InitManager();
    REQUIRE(StartWifiAccessPoint());
    REQUIRE(StopWifiAccessPoint());

    REQUIRE(TestWifiCurrentMode() == WIFI_MODE_STA);
    REQUIRE(TestWifiCalls("esp_wifi_disconnect") == 0);
}

TEST_CASE("an open network is joined with open authentication", "[T-6][FR-14]")
{
    InitManager();
    REQUIRE(ConnectWifiStation(MakeCredentials("cafe", "")));

    const wifi_sta_config_t &sta = TestWifiLastStaConfig()->sta;
    REQUIRE(ToString(sta.ssid, sizeof(sta.ssid)) == "cafe");
    REQUIRE(sta.threshold.authmode == WIFI_AUTH_OPEN);
    REQUIRE(TestWifiCalls("esp_wifi_connect") == 1);
}

TEST_CASE("a secured network is joined with at least WPA2-PSK", "[T-6][FR-14]")
{
    InitManager();
    REQUIRE(ConnectWifiStation(MakeCredentials("home", "correct horse")));

    const wifi_sta_config_t &sta = TestWifiLastStaConfig()->sta;
    REQUIRE(ToString(sta.ssid, sizeof(sta.ssid)) == "home");
    REQUIRE(ToString(sta.password, sizeof(sta.password)) == "correct horse");
    REQUIRE(sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
    REQUIRE(sta.pmf_cfg.capable);
    REQUIRE_FALSE(sta.pmf_cfg.required);
}

TEST_CASE("a 64-digit hex key and a 32-byte SSID fit the driver structure", "[T-6][FR-14]")
{
    InitManager();
    const std::string ssid(32, 's');
    const std::string key(64, 'f');
    REQUIRE(ConnectWifiStation(MakeCredentials(ssid.c_str(), key.c_str())));

    const wifi_sta_config_t &sta = TestWifiLastStaConfig()->sta;
    REQUIRE(std::string(reinterpret_cast<const char *>(sta.ssid), 32) == ssid);
    REQUIRE(std::string(reinterpret_cast<const char *>(sta.password), 64) == key);
}

TEST_CASE("invalid credentials never reach the driver", "[T-6][FR-14]")
{
    InitManager();
    REQUIRE_FALSE(ConnectWifiStation(MakeCredentials("", "")));
    REQUIRE_FALSE(ConnectWifiStation(MakeCredentials("net", "short")));
    REQUIRE_FALSE(ConnectWifiStation(nullptr));
    REQUIRE(TestWifiCalls("esp_wifi_connect") == 0);
    REQUIRE(TestWifiCalls("esp_wifi_set_config") == 0);
}

TEST_CASE("a connection attempt does not drop the AP", "[T-6][FR-15]")
{
    InitManager();
    REQUIRE(StartWifiAccessPoint());
    const int set_mode_calls = TestWifiCalls("esp_wifi_set_mode");

    REQUIRE(ConnectWifiStation(MakeCredentials("home", "correct horse")));

    REQUIRE(TestWifiCurrentMode() == WIFI_MODE_APSTA);
    REQUIRE(TestWifiCalls("esp_wifi_set_mode") == set_mode_calls);   // mode untouched: AP stays up
    REQUIRE(TestWifiCalls("esp_wifi_start") == 1);                   // driver already running
}

TEST_CASE("station-only connect starts the driver once", "[T-9][FR-6]")
{
    InitManager();
    REQUIRE(ConnectWifiStation(MakeCredentials("home", "correct horse")));
    REQUIRE(TestWifiCurrentMode() == WIFI_MODE_STA);
    REQUIRE(ConnectWifiStation(MakeCredentials("home", "correct horse")));

    REQUIRE(TestWifiCalls("esp_wifi_start") == 1);
    REQUIRE(TestWifiCalls("esp_wifi_connect") == 2);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("a driver failure while connecting is reported", "[T-9][FR-6]")
{
    InitManager();
    TestWifiFailCall("esp_wifi_connect");
    REQUIRE_FALSE(ConnectWifiStation(MakeCredentials("home", "correct horse")));
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("the SSID of the network is never logged above Debug and the password never", "[T-7][FR-19][NFR-2]")
{
    InitManager();
    REQUIRE(ConnectWifiStation(MakeCredentials("MyHomeNet", "Sup3rSecretPw!")));
    TestWifiFailCall("esp_wifi_connect");
    ConnectWifiStation(MakeCredentials("MyHomeNet", "Sup3rSecretPw!"));

    const std::string log = TestLogText();
    REQUIRE(log.find("Sup3rSecretPw!") == std::string::npos);
    for (size_t at = log.find("MyHomeNet"); at != std::string::npos; at = log.find("MyHomeNet", at + 1)) {
        const size_t line_start = log.rfind('\n', at);
        const std::string line = log.substr(line_start == std::string::npos ? 0 : line_start + 1, 4);
        REQUIRE(line == "[L0 ");   // only Debug lines may name the network
    }
}

TEST_CASE("disconnect is a no-op before the driver starts, and drops the link afterwards", "[T-7][FR-23]")
{
    InitManager();
    DisconnectWifiStation();
    REQUIRE(TestWifiCalls("esp_wifi_disconnect") == 0);

    REQUIRE(ConnectWifiStation(MakeCredentials("home", "correct horse")));
    DisconnectWifiStation();
    REQUIRE(TestWifiCalls("esp_wifi_disconnect") == 1);
}

TEST_CASE("a scan returns sorted, unique, escaped-ready entries with auth flags", "[T-5][FR-11][FR-13]")
{
    InitManager();
    const wifi_ap_record_t records[] = {
        MakeRecord("weak", -85, WIFI_AUTH_WPA2_PSK),
        MakeRecord("cafe", -40, WIFI_AUTH_OPEN),
        MakeRecord("corp", -60, WIFI_AUTH_WPA2_ENTERPRISE),
        MakeRecord("weak", -50, WIFI_AUTH_WPA2_PSK),
        MakeRecord("", -30, WIFI_AUTH_OPEN),
        MakeRecord("wpa3", -70, WIFI_AUTH_WPA3_ENTERPRISE),
        MakeRecord("mixed", -75, WIFI_AUTH_WPA2_WPA3_PSK),
    };
    TestWifiSetScanRecords(records, 7);

    wifi_scan_entry_t entries[WIFI_SCAN_MAX_ENTRIES] = {};
    REQUIRE(ScanWifiNetworks(entries, WIFI_SCAN_MAX_ENTRIES) == 5);

    REQUIRE(std::string(entries[0].ssid) == "cafe");
    REQUIRE(entries[0].is_open);
    REQUIRE_FALSE(entries[0].is_unsupported);
    REQUIRE(std::string(entries[1].ssid) == "weak");
    REQUIRE(entries[1].rssi_dbm == -50);                 // duplicate merged, strongest kept
    REQUIRE(std::string(entries[2].ssid) == "corp");
    REQUIRE(entries[2].is_unsupported);
    REQUIRE(std::string(entries[3].ssid) == "wpa3");
    REQUIRE(entries[3].is_unsupported);
    REQUIRE(std::string(entries[4].ssid) == "mixed");
    REQUIRE_FALSE(entries[4].is_unsupported);
    REQUIRE_FALSE(entries[4].is_open);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("a scan returns at most 20 networks", "[T-5][FR-11]")
{
    InitManager();
    std::vector<wifi_ap_record_t> records;
    for (int index = 0; index < 40; ++index) {
        records.push_back(MakeRecord(("net" + std::to_string(index)).c_str(), -95 + index, WIFI_AUTH_WPA2_PSK));
    }
    TestWifiSetScanRecords(records.data(), 40);

    wifi_scan_entry_t entries[WIFI_SCAN_MAX_ENTRIES] = {};
    REQUIRE(ScanWifiNetworks(entries, WIFI_SCAN_MAX_ENTRIES) == 20);
    REQUIRE(std::string(entries[0].ssid) == "net39");
    REQUIRE(ScanWifiNetworks(entries, 5) == 5);
}

TEST_CASE("a scan is blocking, skips hidden networks and is bounded below 10 s", "[T-5][FR-12]")
{
    InitManager();
    wifi_scan_entry_t entries[WIFI_SCAN_MAX_ENTRIES] = {};
    REQUIRE(ScanWifiNetworks(entries, WIFI_SCAN_MAX_ENTRIES) == 0);

    const wifi_scan_config_t *config = TestWifiLastScanConfig();
    REQUIRE(TestWifiLastScanBlocking() == 1);
    REQUIRE_FALSE(config->show_hidden);
    REQUIRE(config->scan_time.active.max >= config->scan_time.active.min);
    REQUIRE(config->scan_time.active.max * 13 <= WIFI_SCAN_TIMEOUT_MS);   // 13 channels
}

TEST_CASE("a scan that cannot run reports failure and releases the mutex", "[T-5][T-8][FR-12][NFR-7]")
{
    InitManager();
    wifi_scan_entry_t entries[WIFI_SCAN_MAX_ENTRIES] = {};

    SECTION("busy: a connection attempt is in progress") {
        TestWifiFailCall("esp_wifi_scan_start");
    }
    SECTION("results cannot be read") {
        TestWifiFailCall("esp_wifi_scan_get_ap_records");
    }
    REQUIRE(ScanWifiNetworks(entries, WIFI_SCAN_MAX_ENTRIES) == -1);
    REQUIRE(MockGetMutexBalance() == 0);
    REQUIRE(TestLogCount(2) >= 1);
}

TEST_CASE("the AP address comes from the AP interface", "[T-4][FR-9]")
{
    Reset();
    TestWifiSetApAddress(0x0104A8C0);
    REQUIRE(GetWifiAccessPointAddress() == 0);      // before init there is no interface

    REQUIRE(InitWifiManager(OnEvent));
    REQUIRE(GetWifiAccessPointAddress() == 0x0104A8C0);   // 192.168.4.1 in network byte order
}

TEST_CASE("the AP's DHCP server offers the AP address as DNS server", "[T-4][FR-9][FR-10]")
{
    Reset();
    TestWifiSetApAddress(0x0104A8C0);      // 192.168.4.1

    REQUIRE(InitWifiManager(OnEvent));

    REQUIRE(TestWifiDhcpsOptionCalls() == 1);
    REQUIRE(TestWifiDhcpsOptionMode() == ESP_NETIF_OP_SET);
    REQUIRE(TestWifiDhcpsOptionId() == ESP_NETIF_DOMAIN_NAME_SERVER);
    REQUIRE((TestWifiDhcpsOptionValue() & OFFER_DNS) != 0);              // "offer DNS" flag
    REQUIRE(TestWifiDnsInfoCalls() == 1);
    REQUIRE(TestWifiDnsInfoAddress() == 0x0104A8C0);                     // the wildcard DNS server is the AP itself
}

TEST_CASE("the DNS offer is configured before the AP starts", "[T-4][FR-9]")
{
    InitManager();
    REQUIRE(StartWifiAccessPoint());
    REQUIRE(TestWifiCallOrder("esp_netif_dhcps_option") > 0);
    REQUIRE(TestWifiCallOrder("esp_netif_set_dns_info") > 0);
    REQUIRE(TestWifiCallOrder("esp_netif_dhcps_option") < TestWifiCallOrder("esp_wifi_start"));
    REQUIRE(TestWifiCallOrder("esp_netif_set_dns_info") < TestWifiCallOrder("esp_wifi_start"));
    REQUIRE(TestWifiDhcpsOptionCalls() == 1);                            // not repeated on every AP start
}

TEST_CASE("a failed DNS offer is reported but does not stop start-up", "[T-4][FR-9]")
{
    Reset();
    SECTION("option cannot be set") {
        TestWifiFailCall("esp_netif_dhcps_option");
    }
    SECTION("DNS address cannot be set") {
        TestWifiFailCall("esp_netif_set_dns_info");
    }
    REQUIRE(InitWifiManager(OnEvent));
    REQUIRE(TestLogCount(2) >= 1);
}

TEST_CASE("only open and WPA2/WPA3 personal networks are supported", "[T-5][FR-13]")
{
    InitManager();
    const struct { const char *ssid; wifi_auth_mode_t auth; bool unsupported; } cases[] = {
        {"open", WIFI_AUTH_OPEN, false},
        {"wpa2", WIFI_AUTH_WPA2_PSK, false},
        {"wpa12", WIFI_AUTH_WPA_WPA2_PSK, false},
        {"wpa3", WIFI_AUTH_WPA3_PSK, false},
        {"wpa23", WIFI_AUTH_WPA2_WPA3_PSK, false},
        {"wep", WIFI_AUTH_WEP, true},
        {"wpa1", WIFI_AUTH_WPA_PSK, true},
        {"owe", WIFI_AUTH_OWE, true},
        {"wapi", WIFI_AUTH_WAPI_PSK, true},
        {"ent2", WIFI_AUTH_WPA2_ENTERPRISE, true},
        {"ent3", WIFI_AUTH_WPA3_ENTERPRISE, true},
        {"ent23", WIFI_AUTH_WPA2_WPA3_ENTERPRISE, true},
        {"ent1", WIFI_AUTH_WPA_ENTERPRISE, true},
        {"ent192", WIFI_AUTH_WPA3_ENT_192, true},
    };
    std::vector<wifi_ap_record_t> records;
    int rssi_dbm = -30;
    for (const auto &c : cases) {
        records.push_back(MakeRecord(c.ssid, rssi_dbm--, c.auth));
    }
    TestWifiSetScanRecords(records.data(), static_cast<uint16_t>(records.size()));

    wifi_scan_entry_t entries[WIFI_SCAN_MAX_ENTRIES] = {};
    REQUIRE(ScanWifiNetworks(entries, WIFI_SCAN_MAX_ENTRIES) == static_cast<int>(records.size()));
    for (size_t index = 0; index < records.size(); ++index) {
        INFO(entries[index].ssid);
        REQUIRE(std::string(entries[index].ssid) == cases[index].ssid);
        REQUIRE(entries[index].is_unsupported == cases[index].unsupported);
        REQUIRE(entries[index].is_open == (cases[index].auth == WIFI_AUTH_OPEN));
    }
}
