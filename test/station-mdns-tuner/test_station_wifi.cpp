/**
 * @file test_station_wifi.cpp
 * @brief SPEC-005 T-1, wifi_manager part (FR-1, FR-2, FR-29 GetWifiStationAddress()).
 *
 * wifi_manager.c is compiled through test/captive-portal/mocks/wifi_manager_harness.c against the FFF fakes of the
 * Wi-Fi, netif and event APIs in wifi_fakes.c, which keep one registered handler per event base (WIFI_EVENT and
 * IP_EVENT), so the IP_EVENT registration can no longer overwrite the Wi-Fi handler.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "freertos_mock.h"
#include "host_stubs.h"
#include "wifi_fakes.h"
#include "wifi_manager.h"
void HarnessResetWifiManager(void);
}

namespace {

std::vector<wifi_manager_event_t> g_events;

void OnEvent(wifi_manager_event_t event) { g_events.push_back(event); }

uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t octets[4] = {a, b, c, d};
    uint32_t address = 0;
    std::memcpy(&address, octets, sizeof(address));
    return address;
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
    TestLogReset();
}

std::string Log() { return TestLogText(); }

}  // namespace

TEST_CASE("init registers IP_EVENT_STA_GOT_IP alongside the Wi-Fi events", "[T-1][FR-1]")
{
    Reset();
    REQUIRE(InitWifiManager(OnEvent));
    REQUIRE(TestWifiHandlerRegistered());                          // WIFI_EVENT handler kept
    REQUIRE(TestWifiIpHandlerRegistered());
    REQUIRE(TestWifiIpHandlerEventId() == IP_EVENT_STA_GOT_IP);    // only got-IP, not ESP_EVENT_ANY_ID
    REQUIRE(TestWifiCalls("esp_event_handler_instance_register") == 2);   // once each
}

TEST_CASE("a failed IP_EVENT registration fails init with an Error", "[T-1][FR-1]")
{
    Reset();
    TestWifiFailCall("esp_event_handler_instance_register(IP_EVENT)");
    REQUIRE_FALSE(InitWifiManager(OnEvent));
    REQUIRE(TestWifiHandlerRegistered());                          // the Wi-Fi registration had succeeded
    REQUIRE_FALSE(TestWifiIpHandlerRegistered());
    REQUIRE(Log().find("[L3 wifi] IP event handler registration failed") != std::string::npos);
}

TEST_CASE("got-IP reports WIFI_MANAGER_EVENT_STA_GOT_IP, never a disconnect", "[T-1][FR-1]")
{
    InitManager();
    TestWifiFireGotIp(Ipv4(192, 168, 1, 42), 1);
    REQUIRE(g_events == std::vector<wifi_manager_event_t>{WIFI_MANAGER_EVENT_STA_GOT_IP});
}

TEST_CASE("the three events are reported separately and in order", "[T-1][FR-1]")
{
    InitManager();
    TestWifiFireEvent(WIFI_EVENT_STA_CONNECTED, nullptr);
    TestWifiFireGotIp(Ipv4(192, 168, 1, 42), 1);
    wifi_event_sta_disconnected_t lost = {WIFI_REASON_BEACON_TIMEOUT};
    TestWifiFireEvent(WIFI_EVENT_STA_DISCONNECTED, &lost);
    TestWifiFireEvent(WIFI_EVENT_STA_CONNECTED, nullptr);
    TestWifiFireGotIp(Ipv4(192, 168, 1, 77), 1);

    REQUIRE(g_events == std::vector<wifi_manager_event_t>{
                            WIFI_MANAGER_EVENT_STA_CONNECTED, WIFI_MANAGER_EVENT_STA_GOT_IP,
                            WIFI_MANAGER_EVENT_STA_DISCONNECTED, WIFI_MANAGER_EVENT_STA_CONNECTED,
                            WIFI_MANAGER_EVENT_STA_GOT_IP});
}

TEST_CASE("got-IP logs the exact FR-2 Info line with the address and fallback URL", "[T-1][FR-2]")
{
    InitManager();
    TestWifiFireGotIp(Ipv4(192, 168, 1, 42), 1);
    REQUIRE(Log() == "[L1 wifi] station IP address 192.168.1.42, fallback URL http://192.168.1.42/\n");
    REQUIRE(TestLogCount(1) == 1);
}

TEST_CASE("every got-IP logs one FR-2 line, with the new address each time", "[T-1][FR-2]")
{
    InitManager();
    TestWifiFireGotIp(Ipv4(192, 168, 1, 42), 1);
    TestWifiFireGotIp(Ipv4(10, 0, 0, 7), 1);
    TestWifiFireGotIp(Ipv4(10, 0, 0, 7), 1);
    REQUIRE(Log() == "[L1 wifi] station IP address 192.168.1.42, fallback URL http://192.168.1.42/\n"
                     "[L1 wifi] station IP address 10.0.0.7, fallback URL http://10.0.0.7/\n"
                     "[L1 wifi] station IP address 10.0.0.7, fallback URL http://10.0.0.7/\n");
    REQUIRE(g_events.size() == 3);
}

TEST_CASE("the longest FR-2 line is at most 72 characters (within the 127-character limit)", "[T-1][FR-2]")
{
    InitManager();
    TestWifiFireGotIp(Ipv4(255, 255, 255, 255), 1);
    const std::string line = Log();
    const std::string message = line.substr(std::strlen("[L1 wifi] "), line.size() - std::strlen("[L1 wifi] ") - 1);
    REQUIRE(message == "station IP address 255.255.255.255, fallback URL http://255.255.255.255/");
    CAPTURE(message.size());
    REQUIRE(message.size() <= 72);
    REQUIRE(message.size() <= 127);
}

TEST_CASE("a got-IP without payload still reports the event and does not crash", "[T-1][FR-1]")
{
    InitManager();
    TestWifiFireGotIp(0, 0);
    REQUIRE(g_events == std::vector<wifi_manager_event_t>{WIFI_MANAGER_EVENT_STA_GOT_IP});
    REQUIRE(Log().find("station IP address") == std::string::npos);
}

TEST_CASE("unrelated Wi-Fi events are not reported (no mis-mapping)", "[T-1][FR-1]")
{
    InitManager();
    TestWifiFireEvent(99, nullptr);
    REQUIRE(g_events.empty());
}

TEST_CASE("GetWifiStationAddress() returns the station netif address, or 0 when there is none", "[T-1][FR-29]")
{
    Reset();
    TestWifiSetStaAddress(Ipv4(192, 168, 1, 42));
    TestWifiSetApAddress(Ipv4(192, 168, 4, 1));
    REQUIRE(GetWifiStationAddress() == 0);                         // before init there is no netif

    REQUIRE(InitWifiManager(OnEvent));
    REQUIRE(GetWifiStationAddress() == Ipv4(192, 168, 1, 42));
    REQUIRE(TestWifiLastIpInfoNetif() == TestWifiStaNetif());      // read from the STA netif, not the AP one
    REQUIRE(GetWifiAccessPointAddress() == Ipv4(192, 168, 4, 1));  // the AP query is unchanged

    TestWifiSetStaAddress(0);                                      // no address assigned yet
    REQUIRE(GetWifiStationAddress() == 0);

    TestWifiSetStaAddress(Ipv4(10, 0, 0, 7));                      // follows the current address
    REQUIRE(GetWifiStationAddress() == Ipv4(10, 0, 0, 7));

    TestWifiFailCall("esp_netif_get_ip_info");                    // a netif query error reads as "none"
    REQUIRE(GetWifiStationAddress() == 0);
}
