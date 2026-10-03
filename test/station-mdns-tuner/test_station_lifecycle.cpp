/**
 * @file test_station_lifecycle.cpp
 * @brief SPEC-005 T-1 (orchestrator mapping), T-2 (FR-3..FR-10, FR-29, NFR-4, NFR-13) and the host part of NFR-5:
 *        the station-services lifecycle of the provisioning orchestrator.
 *
 * provisioning.c runs as a coroutine on the fake millisecond clock of test/captive-portal (freertos_mock), with every
 * component it calls replaced by the FFF fakes of test/captive-portal/mocks/provisioning_fakes.c. For SPEC-005 those
 * fakes add StartHttpStationServer, SetHttpStationIdentity, GetWifiStationAddress, StartMdnsService, StopMdnsService,
 * LogMdnsHostnameInUse and the heap queries, and model which HTTP profile and whether mDNS would be running, so
 * overlaps (FR-7) and unbalanced starts/stops (NFR-5) are observable.
 *
 * The mock clock runs one tick per millisecond; NFR-4's "at least 2 ticks at 100 Hz" is checked on the constants.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "freertos_mock.h"
#include "host_stubs.h"
#include "provisioning.h"
#include "provisioning_fakes.h"
void HarnessResetProvisioning(void);
const char *HarnessGetStateName(void);
const http_portal_ops_t *HarnessGetPortalOps(void);
bool HarnessHasIp(void);
bool HarnessIsStationHttpUp(void);
bool HarnessIsMdnsUp(void);
const char *HarnessGetHostnameInUse(void);
uint32_t HarnessGetHostnameCheckDelayMs(void);
uint32_t HarnessGetStationServiceRetryMs(void);
}

namespace {

wifi_credentials_t MakeCredentials(const char *ssid, const char *password)
{
    wifi_credentials_t credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t octets[4] = {a, b, c, d};
    uint32_t address = 0;
    std::memcpy(&address, octets, sizeof(address));
    return address;
}

const wifi_credentials_t kStored = MakeCredentials("stored-net", "stored-password");
const wifi_credentials_t kNew = MakeCredentials("new-net", "Sup3rSecretPw!");
const uint32_t kIp = Ipv4(192, 168, 1, 42);
const uint32_t kNewIp = Ipv4(192, 168, 1, 77);
constexpr uint32_t kCheckMs = 3000;
constexpr uint32_t kRetryMs = 5000;

// ---- Simulation helpers (as in test/captive-portal/test_provisioning.cpp) -----------------------------------------

void Run(uint32_t until_ms) { MockRunTask(0, until_ms); }
void Settle() { Run(MockGetNowMs()); }
uint32_t Now() { return MockGetNowMs(); }

void Boot(const wifi_credentials_t *stored)
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    TestFakesSetStored(stored);
    ProvisioningStart();
    Run(0);
}

void Emit(wifi_manager_event_t event) { TestFakesWifiCallback()(event); Settle(); }
void StationConnected() { Emit(WIFI_MANAGER_EVENT_STA_CONNECTED); }
void StationDisconnected() { Emit(WIFI_MANAGER_EVENT_STA_DISCONNECTED); }
void GotIp() { Emit(WIFI_MANAGER_EVENT_STA_GOT_IP); }
void PressButton() { TestFakesButtonCallback()(); Settle(); }
bool Submit(const wifi_credentials_t &credentials)
{
    bool accepted = TestFakesPortalOps()->submit_credentials(&credentials);
    Settle();
    return accepted;
}

std::string State() { return HarnessGetStateName(); }
int Calls(const char *name) { return TestCallCount(name); }
int Pos(const char *name, int index) { return TestCallPosition(name, index); }
std::string Log() { return TestLogText(); }
bool LogHas(const std::string &text) { return Log().find(text) != std::string::npos; }
size_t LogCount(const std::string &text)
{
    size_t count = 0;
    const std::string log = Log();
    for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++count;
    return count;
}

/** Boot with stored credentials and associate at t = 3,000 ms; no IP yet. */
void BootAssociated(uint32_t station_ipv4 = kIp)
{
    Boot(&kStored);
    TestFakesSetStationAddress(station_ipv4);
    Run(3000);
    StationConnected();
    REQUIRE(State() == "CONNECTED");
}

/** Boot, associate at 3,000 ms and get an IP at 3,500 ms: station services up. */
void BootWithServices(uint32_t station_ipv4 = kIp)
{
    BootAssociated(station_ipv4);
    Run(3500);
    GotIp();
    REQUIRE(HarnessIsStationHttpUp());
    REQUIRE(HarnessIsMdnsUp());
}

/** From station operation: GPIO9 → portal → submit kNew → trial associates [→ got-IP] → 3 s → station. */
void RoundTripThroughProvisioning(bool with_ip)
{
    Run(Now() + 1000);
    PressButton();
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Submit(kNew));
    StationConnected();
    REQUIRE(State() == "PORTAL_SUCCESS");
    if (with_ip) {
        GotIp();
    }
    Run(Now() + 3000);
    REQUIRE(State() == "CONNECTED");
}

void RequireNoOverlap()
{
    REQUIRE(TestProfileOverlapCount() == 0);
    if (TestHttpProfile() == TEST_HTTP_PORTAL) {
        REQUIRE_FALSE(TestMdnsRunning());
    }
}

}  // namespace

// ==== T-1: explicit three-way event mapping (FR-1, FR-3) ============================================================

TEST_CASE("got-IP is mapped to MSG_STA_GOT_IP, never to a disconnect", "[T-1][FR-1][FR-3]")
{
    BootAssociated();
    const int connects = Calls("ConnectWifiStation");
    GotIp();
    REQUIRE(State() == "CONNECTED");                               // a disconnect would start the reconnect backoff
    REQUIRE(HarnessHasIp());
    REQUIRE_FALSE(LogHas("connection lost"));
    Run(Now() + 120000);
    REQUIRE(Calls("ConnectWifiStation") == connects);              // no reconnect attempt ever
}

TEST_CASE("connected and disconnected keep their SPEC-002 mapping", "[T-1][FR-1]")
{
    BootWithServices();
    StationDisconnected();
    REQUIRE(State() == "RECONNECT_WAIT");
    REQUIRE_FALSE(HarnessHasIp());                                 // FR-3: cleared on disconnect
    Run(Now() + 1000);
    REQUIRE(State() == "RECONNECT_TRY");
    StationConnected();
    REQUIRE(State() == "CONNECTED");
}

TEST_CASE("an unknown wifi_manager event is not mis-mapped to a disconnect", "[T-1][FR-1]")
{
    BootWithServices();
    Emit(static_cast<wifi_manager_event_t>(42));
    REQUIRE(State() == "CONNECTED");
    REQUIRE(HarnessHasIp());
    REQUIRE_FALSE(LogHas("connection lost"));
}

TEST_CASE("has_ip is set on got-IP in any state and cleared by EnterProvisioning()", "[T-2][FR-3]")
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(State() == "PORTAL_IDLE");
    GotIp();
    REQUIRE(HarnessHasIp());                                       // set in a portal state too

    BootWithServices();
    PressButton();
    REQUIRE_FALSE(HarnessHasIp());
}

// ==== T-2 (a): boot → connected → got-IP ============================================================================

TEST_CASE("(a) got-IP in STATE_CONNECTED sets the identity, then starts HTTP, then mDNS, once each", "[T-2][FR-4][FR-8][FR-29]")
{
    BootAssociated();
    Run(3500);
    REQUIRE(Calls("StartHttpStationServer") == 0);                 // association alone starts nothing
    GotIp();

    REQUIRE(Calls("SetHttpStationIdentity") == 1);
    REQUIRE(Calls("StartHttpStationServer") == 1);
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(Pos("SetHttpStationIdentity", 0) < Pos("StartHttpStationServer", 0));
    REQUIRE(Pos("StartHttpStationServer", 0) < Pos("StartMdnsService", 0));
    REQUIRE(std::string(TestIdentityNameAt(0)) == "rgb-tuner");    // FR-29 a: default name, current IP
    REQUIRE(TestIdentityAddressAt(0) == kIp);
    REQUIRE(TestCallTimeMs("StartHttpStationServer", 0) == 3500);
    REQUIRE(StartHttpStationServer_fake.arg0_val == HarnessGetPortalOps());
    REQUIRE(HarnessGetPortalOps()->apply_led_timing != nullptr);
    REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
    REQUIRE(TestMdnsRunning());
}

TEST_CASE("(a) association without got-IP never starts the station services", "[T-2][FR-4]")
{
    BootAssociated();
    Run(Now() + 600000);
    REQUIRE(Calls("StartHttpStationServer") == 0);
    REQUIRE(Calls("StartMdnsService") == 0);
    REQUIRE(Calls("SetHttpStationIdentity") == 0);
}

TEST_CASE("(j) the hostname check runs once, 3,000 ms after the start", "[T-2][FR-8][FR-21]")
{
    BootWithServices();
    Run(3500 + kCheckMs - 1);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 0);
    Run(3500 + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
    REQUIRE(TestCallTimeMs("LogMdnsHostnameInUse", 0) == 3500 + kCheckMs);
    Run(Now() + 600000);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);                   // the deadline is cleared afterwards
    REQUIRE(State() == "CONNECTED");
}

TEST_CASE("(m) a check that returns the default name refreshes the identity with it", "[T-2][FR-29]")
{
    BootWithServices();
    Run(3500 + kCheckMs);
    REQUIRE(Calls("SetHttpStationIdentity") == 2);                 // (a) + (c)
    REQUIRE(std::string(TestIdentityNameAt(1)) == "rgb-tuner");
    REQUIRE(TestIdentityAddressAt(1) == kIp);
    REQUIRE(Pos("LogMdnsHostnameInUse", 0) < Pos("SetHttpStationIdentity", 1));
}

TEST_CASE("(m) a renamed host from the check is stored and passed to the identity", "[T-2][FR-21][FR-29]")
{
    BootWithServices();
    TestFakesSetHostnameInUse("rgb-tuner-2");
    Run(3500 + kCheckMs);
    REQUIRE(Calls("SetHttpStationIdentity") == 2);
    REQUIRE(std::string(TestIdentityNameAt(1)) == "rgb-tuner-2");  // FR-29 c
    REQUIRE(TestIdentityAddressAt(1) == kIp);
    REQUIRE(std::string(HarnessGetHostnameInUse()) == "rgb-tuner-2");   // s_hostname_in_use updated

    TestFakesSetStationAddress(kNewIp);                            // a later got-IP keeps the renamed name (FR-29 b)
    GotIp();
    REQUIRE(Calls("SetHttpStationIdentity") == 3);
    REQUIRE(std::string(TestIdentityNameAt(2)) == "rgb-tuner-2");
    REQUIRE(TestIdentityAddressAt(2) == kNewIp);
}

TEST_CASE("(m) a failed check does not call the identity setter", "[T-2][FR-21][FR-29]")
{
    BootWithServices();
    TestFakesSetHostnameInUse(nullptr);                            // LogMdnsHostnameInUse() returns false
    Run(3500 + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
    REQUIRE(Calls("SetHttpStationIdentity") == 1);                 // only the (a) write
    REQUIRE(std::string(HarnessGetHostnameInUse()) == "rgb-tuner");
}

// ==== T-2 (b), (c): no restart, keep running (FR-4, FR-5, FR-8, FR-29 b) ==========================================

TEST_CASE("(b) a second got-IP does not restart the services, updates the IP and re-arms the check", "[T-2][FR-4][FR-8][FR-29]")
{
    BootWithServices();
    Run(5000);
    TestFakesSetStationAddress(kNewIp);
    GotIp();

    REQUIRE(Calls("StartHttpStationServer") == 1);
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(Calls("SetHttpStationIdentity") == 2);                 // FR-29 b with the new address
    REQUIRE(std::string(TestIdentityNameAt(1)) == "rgb-tuner");
    REQUIRE(TestIdentityAddressAt(1) == kNewIp);

    Run(3500 + kCheckMs);                                          // the first check was re-armed, not run
    REQUIRE(Calls("LogMdnsHostnameInUse") == 0);
    Run(5000 + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
}

TEST_CASE("(c) disconnect and reconnect never stop or restart the services", "[T-2][FR-5]")
{
    BootWithServices();
    for (int outage = 0; outage < 3; ++outage) {
        INFO("outage " << outage);
        Run(Now() + 500);
        StationDisconnected();
        REQUIRE(State() == "RECONNECT_WAIT");
        Run(Now() + 30000);                                        // a 30 s router outage: several backoff cycles
        StationConnected();
        REQUIRE(State() == "CONNECTED");
        TestFakesSetStationAddress(outage % 2 == 0 ? kNewIp : kIp);
        GotIp();
    }
    REQUIRE(Calls("StopMdnsService") == 0);
    REQUIRE(Calls("StopHttpPortal") == 0);
    REQUIRE(Calls("StartHttpStationServer") == 1);
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE_FALSE(LogHas("station services stopped"));
    REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
    REQUIRE(TestMdnsRunning());
}

TEST_CASE("(c) the hostname check runs again after each reconnection's got-IP", "[T-2][FR-8]")
{
    BootWithServices();
    Run(3500 + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);

    StationDisconnected();
    Run(Now() + 1000);
    StationConnected();
    const uint32_t got_ip_ms = Now() + 200;
    Run(got_ip_ms);
    GotIp();
    Run(got_ip_ms + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 2);
    REQUIRE(TestCallTimeMs("LogMdnsHostnameInUse", 1) == got_ip_ms + kCheckMs);
}

TEST_CASE("a disconnect before the check cancels it", "[T-2][FR-8]")
{
    BootWithServices();
    Run(4000);
    StationDisconnected();
    Run(3500 + kCheckMs + 10);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 0);
}

// ==== T-2 (d), (h): EnterProvisioning() stops mDNS then HTTP first, for every trigger (FR-6, FR-7) ================

namespace {

/** FR-6: StopMdnsService() then StopHttpPortal() are the first calls of the first EnterProvisioning(). */
void RequireStopsFirst()
{
    const int mdns = Pos("StopMdnsService", 0);
    const int http = Pos("StopHttpPortal", 0);
    REQUIRE(mdns >= 0);
    REQUIRE(http >= 0);
    REQUIRE(mdns < http);
    for (const char *later : {"DisconnectWifiStation", "StartWifiAccessPoint", "StartDnsServer", "StartHttpPortal"}) {
        INFO(later);
        const int position = Pos(later, Calls(later) - 1);
        REQUIRE(http < position);
    }
}

}  // namespace

TEST_CASE("(d) GPIO9 from STATE_CONNECTED stops mDNS, then HTTP, before anything else", "[T-2][FR-6][FR-7]")
{
    BootWithServices();
    const int disconnects = Calls("DisconnectWifiStation");
    PressButton();

    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("StopMdnsService") == 1);
    REQUIRE(Pos("StopMdnsService", 0) < Pos("StopHttpPortal", 0));
    REQUIRE(Pos("StopHttpPortal", 0) < Pos("DisconnectWifiStation", disconnects));
    REQUIRE(Pos("StopHttpPortal", 0) < Pos("StartWifiAccessPoint", 0));
    REQUIRE(LogHas("[L1 provision] station services stopped"));
    REQUIRE(Log().find("station services stopped") < Log().find("entering provisioning mode"));
    REQUIRE_FALSE(TestMdnsRunning());
    REQUIRE(TestHttpProfile() == TEST_HTTP_PORTAL);
    RequireNoOverlap();
}

TEST_CASE("(d) GPIO9 from STATE_RECONNECT_WAIT stops mDNS, then HTTP, before anything else", "[T-2][FR-6][FR-7]")
{
    BootWithServices();
    StationDisconnected();
    REQUIRE(State() == "RECONNECT_WAIT");
    PressButton();
    REQUIRE(State() == "PORTAL_IDLE");
    RequireStopsFirst();
    REQUIRE(LogHas("station services stopped"));
    RequireNoOverlap();
}

TEST_CASE("(d) GPIO9 from STATE_RECONNECT_TRY stops mDNS, then HTTP, before anything else", "[T-2][FR-6]")
{
    BootWithServices();
    StationDisconnected();
    Run(Now() + 1000);
    REQUIRE(State() == "RECONNECT_TRY");
    PressButton();
    RequireStopsFirst();
    RequireNoOverlap();
}

TEST_CASE("(h) five failed boot attempts enter provisioning with the FR-6 stops first, services never started", "[T-2][FR-6]")
{
    Boot(&kStored);
    Run(59050);
    REQUIRE(State() == "PORTAL_IDLE");
    RequireStopsFirst();
    REQUIRE(Calls("StartHttpStationServer") == 0);
    REQUIRE(Calls("StartMdnsService") == 0);
    REQUIRE_FALSE(LogHas("station services stopped"));            // nothing was running
}

TEST_CASE("no stored credentials enter provisioning with the FR-6 stops first", "[T-2][FR-6]")
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("StopMdnsService") == 1);
    REQUIRE(Pos("StopMdnsService", 0) < Pos("StopHttpPortal", 0));
    REQUIRE(Pos("StopHttpPortal", 0) < Pos("StartWifiAccessPoint", 0));
    REQUIRE_FALSE(LogHas("station services stopped"));
}

TEST_CASE("the STATE_PORTAL_RETRY re-entry also stops mDNS then HTTP first", "[T-2][FR-6]")
{
    Boot(nullptr);
    TestFakesFailStart("StartHttpPortal", 1);
    Run(1050);
    REQUIRE(State() == "PORTAL_RETRY");
    Run(1050 + 5000);
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("StopMdnsService") == 2);
    const int second_entry_mdns = Pos("StopMdnsService", 1);
    REQUIRE(second_entry_mdns < Pos("StartWifiAccessPoint", 1));
    REQUIRE(Pos("StopHttpPortal", 2) == second_entry_mdns + 1);   // entry stop, cleanup stop, then the re-entry
}

TEST_CASE("(g) got-IP in any portal state starts nothing", "[T-2][FR-4][FR-7]")
{
    SECTION("PORTAL_IDLE") {
        BootWithServices();
        PressButton();
        GotIp();
        REQUIRE(State() == "PORTAL_IDLE");
    }
    SECTION("PORTAL_TRIAL") {
        BootWithServices();
        PressButton();
        REQUIRE(Submit(kNew));
        GotIp();
        REQUIRE(State() == "PORTAL_TRIAL");
    }
    SECTION("PORTAL_SUCCESS") {
        BootWithServices();
        PressButton();
        REQUIRE(Submit(kNew));
        StationConnected();
        GotIp();
        REQUIRE(State() == "PORTAL_SUCCESS");
    }
    SECTION("PORTAL_RETRY") {
        BootWithServices();
        TestFakesFailStart("StartHttpPortal", 1);
        PressButton();
        REQUIRE(State() == "PORTAL_RETRY");
        GotIp();
        REQUIRE(State() == "PORTAL_RETRY");
    }
    REQUIRE(Calls("StartHttpStationServer") == 1);                 // only the one from before provisioning
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(HarnessHasIp());
    REQUIRE(LogHas("[L0 provision] got IP in state"));
    REQUIRE_FALSE(TestMdnsRunning());
    RequireNoOverlap();
}

TEST_CASE("the provisioning portal never runs together with mDNS or the station profile", "[T-2][FR-7]")
{
    BootWithServices();
    PressButton();
    REQUIRE(TestHttpProfile() == TEST_HTTP_PORTAL);
    REQUIRE_FALSE(TestMdnsRunning());
    REQUIRE(Pos("StopMdnsService", 0) < Pos("StartHttpPortal", 0));
    RequireNoOverlap();
}

// ==== T-2 (e), (f): leaving provisioning (FR-4 b) ==================================================================

TEST_CASE("(e) provisioning success with an IP starts the services after StopPortalServices()", "[T-2][FR-4][FR-29]")
{
    BootWithServices();
    TestFakesSetStationAddress(kNewIp);
    RoundTripThroughProvisioning(true);

    REQUIRE(Calls("StartHttpStationServer") == 2);
    REQUIRE(Calls("StartMdnsService") == 2);
    const int stop_ap = Pos("StopWifiAccessPoint", 0);             // StopPortalServices(): HTTP, DNS, then the AP
    REQUIRE(Pos("StopDnsServer", 0) < stop_ap);
    REQUIRE(stop_ap < Pos("SetHttpStationIdentity", 1));
    REQUIRE(Pos("SetHttpStationIdentity", 1) < Pos("StartHttpStationServer", 1));
    REQUIRE(Pos("StartHttpStationServer", 1) < Pos("StartMdnsService", 1));
    REQUIRE(std::string(TestIdentityNameAt(1)) == "rgb-tuner");    // FR-29 a again, with the current address
    REQUIRE(TestIdentityAddressAt(1) == kNewIp);
    REQUIRE(Log().find("provisioning mode left") < Log().rfind("station services: http=up mdns=up"));
    REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
    REQUIRE(TestMdnsRunning());
    RequireNoOverlap();

    const uint32_t left_ms = Now();
    Run(left_ms + kCheckMs);                                       // the check is armed after the start
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
    REQUIRE(TestCallTimeMs("LogMdnsHostnameInUse", 0) == left_ms + kCheckMs);
}

TEST_CASE("(e) after a rename, re-entering station mode resets the identity name to the default", "[T-2][FR-29]")
{
    BootWithServices();
    TestFakesSetHostnameInUse("rgb-tuner-2");
    Run(3500 + kCheckMs);
    REQUIRE(std::string(HarnessGetHostnameInUse()) == "rgb-tuner-2");
    RoundTripThroughProvisioning(true);
    REQUIRE(std::string(TestIdentityNameAt(Calls("SetHttpStationIdentity") - 1)) == "rgb-tuner");
}

TEST_CASE("(f) provisioning success without an IP starts the services on the later got-IP", "[T-2][FR-4]")
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(Submit(kNew));
    StationConnected();
    Run(Now() + 3000);
    REQUIRE(State() == "CONNECTED");
    REQUIRE(Calls("StartHttpStationServer") == 0);
    REQUIRE(Calls("StartMdnsService") == 0);

    TestFakesSetStationAddress(kIp);
    Run(Now() + 700);
    GotIp();
    REQUIRE(Calls("StartHttpStationServer") == 1);
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(TestIdentityAddressAt(0) == kIp);
    RequireNoOverlap();
}

TEST_CASE("leaving provisioning without association starts nothing", "[T-2][FR-4]")
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(Submit(kNew));
    StationConnected();
    GotIp();
    StationDisconnected();                                         // lost before the AP shutdown
    Run(Now() + 3000);
    REQUIRE(State() == "RECONNECT_WAIT");
    REQUIRE(Calls("StartHttpStationServer") == 0);
    REQUIRE(Calls("StartMdnsService") == 0);
}

// ==== T-2 (i), (k), (l): start failure, retry, diagnostics (FR-8..FR-10, NFR-4) ===================================

TEST_CASE("(i) an mDNS start failure: Error, then Warning every 5,000 ms, retrying only mDNS", "[T-2][FR-8][FR-9]")
{
    BootAssociated();
    TestFakesFailStart("StartMdnsService", 3);
    Run(3500);
    GotIp();

    REQUIRE(Calls("StartHttpStationServer") == 1);                 // a failure of one does not skip the other
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(LogCount("[L3 provision] station service start failed (service=mdns)") == 1);

    Run(3500 + kRetryMs - 1);
    REQUIRE(Calls("StartMdnsService") == 1);
    Run(3500 + kRetryMs);
    REQUIRE(Calls("StartMdnsService") == 2);
    REQUIRE(TestCallTimeMs("StartMdnsService", 1) == 3500 + kRetryMs);
    REQUIRE(LogCount("[L2 provision] station service start failed (service=mdns)") == 1);

    Run(3500 + 2 * kRetryMs);
    REQUIRE(Calls("StartMdnsService") == 3);
    REQUIRE(LogCount("[L2 provision] station service start failed (service=mdns)") == 2);
    REQUIRE(LogCount("[L3 provision] station service start failed") == 1);   // Error only for the first

    Run(3500 + 3 * kRetryMs);                                      // the fourth attempt succeeds
    REQUIRE(Calls("StartMdnsService") == 4);
    REQUIRE(TestMdnsRunning());
    REQUIRE(Calls("StartHttpStationServer") == 1);                 // HTTP was never restarted
    REQUIRE(Calls("SetHttpStationIdentity") == 1);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 0);

    Run(3500 + 3 * kRetryMs + kCheckMs - 1);                       // success arms the check at +3,000 ms
    REQUIRE(Calls("LogMdnsHostnameInUse") == 0);
    Run(3500 + 3 * kRetryMs + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
    Run(Now() + 60000);
    REQUIRE(Calls("StartMdnsService") == 4);                       // no further retries
}

TEST_CASE("(i) an HTTP start failure retries only HTTP, with the identity set before each attempt", "[T-2][FR-9][FR-29]")
{
    BootAssociated();
    TestFakesFailStart("StartHttpStationServer", 1);
    Run(3500);
    GotIp();
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(TestMdnsRunning());
    REQUIRE(LogHas("[L3 provision] station service start failed (service=http)"));

    Run(3500 + kRetryMs);
    REQUIRE(Calls("StartHttpStationServer") == 2);
    REQUIRE(Calls("StartMdnsService") == 1);
    REQUIRE(Calls("SetHttpStationIdentity") == 2);
    REQUIRE(Pos("SetHttpStationIdentity", 1) < Pos("StartHttpStationServer", 1));
    REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
    Run(3500 + kRetryMs + kCheckMs);
    REQUIRE(Calls("LogMdnsHostnameInUse") == 1);
}

TEST_CASE("(i) both services failing are both retried; the orchestrator never reboots or enters provisioning", "[T-2][FR-9]")
{
    BootAssociated();
    TestFakesFailStart("StartHttpStationServer", 100);
    TestFakesFailStart("StartMdnsService", 100);
    Run(3500);
    GotIp();
    Run(3500 + 10 * kRetryMs);
    REQUIRE(Calls("StartHttpStationServer") == 11);
    REQUIRE(Calls("StartMdnsService") == 11);
    REQUIRE(LogCount("[L3 provision]") == 2);                      // one Error per service
    REQUIRE(LogCount("[L2 provision] station service start failed") == 20);
    REQUIRE(State() == "CONNECTED");
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
}

TEST_CASE("(i) a retry is not attempted after the IP is lost", "[T-2][FR-9]")
{
    BootAssociated();
    TestFakesFailStart("StartMdnsService", 100);
    Run(3500);
    GotIp();
    StationDisconnected();
    Run(3500 + 3 * kRetryMs);
    StationConnected();                                            // associated again, no IP yet
    Run(Now() + 3 * kRetryMs);
    REQUIRE(Calls("StartMdnsService") == 1);
}

TEST_CASE("(k) every start attempt logs the FR-10 Debug resource line", "[T-2][FR-10]")
{
    BootAssociated();
    TestFakesFailStart("StartMdnsService", 1);
    Run(3500);
    GotIp();
    REQUIRE(LogHas("[L0 provision] station services: http=up mdns=down free_heap=180000 min_free_heap=120000 tasks=1"));
    Run(3500 + kRetryMs);
    REQUIRE(LogHas("[L0 provision] station services: http=up mdns=up free_heap=180000 min_free_heap=120000 tasks=1"));
    REQUIRE(LogCount("station services: http=") == 2);
    REQUIRE(esp_get_free_heap_size_fake.call_count == 2);
    REQUIRE(esp_get_minimum_free_heap_size_fake.call_count == 2);
}

TEST_CASE("(l) the new deadlines are multiples of 10 ms and at least 2 ticks at 100 Hz", "[T-2][NFR-4]")
{
    for (uint32_t delay_ms : {HarnessGetHostnameCheckDelayMs(), HarnessGetStationServiceRetryMs()}) {
        INFO(delay_ms);
        REQUIRE(delay_ms % 10 == 0);
        REQUIRE(delay_ms / 10 >= 2);                               // pdMS_TO_TICKS() at CONFIG_FREERTOS_HZ = 100
    }
    REQUIRE(HarnessGetHostnameCheckDelayMs() == 3000);
    REQUIRE(HarnessGetStationServiceRetryMs() == 5000);
}

// ==== Leak check (NFR-5 host part, FR-10) ===========================================================================

TEST_CASE("20 got-IP → provisioning → station cycles leave starts and stops balanced", "[T-2][NFR-5][FR-6][FR-7]")
{
    BootWithServices();
    for (int cycle = 1; cycle <= 20; ++cycle) {
        INFO("cycle " << cycle);
        RoundTripThroughProvisioning(true);
        REQUIRE(TestEffectiveStarts("http_station") == cycle + 1);
        REQUIRE(TestEffectiveStops("http_station") == cycle);
        REQUIRE(TestEffectiveStarts("mdns") == cycle + 1);
        REQUIRE(TestEffectiveStops("mdns") == cycle);
        REQUIRE(TestEffectiveStarts("http_portal") == cycle);
        REQUIRE(TestEffectiveStops("http_portal") == cycle);
        REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
        REQUIRE(TestMdnsRunning());
        RequireNoOverlap();
        REQUIRE(LogCount("station services stopped") == static_cast<size_t>(cycle));
    }
    REQUIRE(MockGetTaskCount() == 1);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("20 outage cycles start nothing and stop nothing", "[T-2][NFR-5][FR-5]")
{
    BootWithServices();
    for (int cycle = 1; cycle <= 20; ++cycle) {
        StationDisconnected();
        Run(Now() + 30000);
        StationConnected();
        GotIp();
    }
    REQUIRE(TestEffectiveStarts("http_station") == 1);
    REQUIRE(TestEffectiveStarts("mdns") == 1);
    REQUIRE(TestEffectiveStops("http_station") == 0);
    REQUIRE(TestEffectiveStops("mdns") == 0);
    REQUIRE(LogCount("station services: http=up mdns=up") == 1);
}
