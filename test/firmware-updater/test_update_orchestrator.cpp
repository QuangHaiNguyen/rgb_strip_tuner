/**
 * @file test_update_orchestrator.cpp
 * @brief SPEC-007 T-5 (orchestrator part, FR-10, FR-11): main/provisioning/provisioning.c on the fake FreeRTOS clock
 *        with every component faked (test/captive-portal/mocks). The update gesture posts MSG_UPDATE_REQUEST, which in
 *        every orchestrator state calls RequestFwUpdate(), stops the HTTP server, mDNS, the portal and Wi-Fi, then
 *        RestartIntoUpdater(); an invalid record also restarts; a failed request keeps running unchanged. The healthy
 *        deadline calls MarkFirmwareHealthy() exactly once at 30,000 ms. (RequestFwUpdate() and MarkFirmwareHealthy()
 *        themselves are tested in test_fw_update.cpp.)
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "freertos_mock.h"
#include "host_stubs.h"
#include "provisioning.h"
#include "provisioning_fakes.h"
void HarnessResetProvisioning(void);
const char *HarnessGetStateName(void);
/* Fakes defined in test/captive-portal/mocks/provisioning_fakes.c and not declared in its header. */
DECLARE_FAKE_VALUE_FUNC(bool, StartButton, button_request_cb_t, button_request_cb_t);
DECLARE_FAKE_VALUE_FUNC(fw_update_request_t, RequestFwUpdate);
DECLARE_FAKE_VOID_FUNC(RestartIntoUpdater);
DECLARE_FAKE_VOID_FUNC(MarkFirmwareHealthy);
DECLARE_FAKE_VALUE_FUNC(bool, StartFwHealthyTimer);
}

namespace {

wifi_credentials_t MakeCredentials(const char *ssid, const char *password)
{
    wifi_credentials_t credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

const wifi_credentials_t kStored = MakeCredentials("stored-net", "stored-password");
const wifi_credentials_t kNew = MakeCredentials("new-net", "Sup3rSecretPw!");

void Run(uint32_t until_ms) { MockRunTask(0, until_ms); }
void Settle() { Run(MockGetNowMs()); }
std::string State() { return HarnessGetStateName(); }

/** State of the services when RestartIntoUpdater() was called. */
struct RestartSnapshot {
    int calls = 0;
    test_http_profile_t http = TEST_HTTP_NONE;
    bool mdns = false;
    int ap_stops = 0;
    int disconnects = 0;
    int request_calls = 0;
} g_restart;

void RestartFake()
{
    g_restart.calls++;
    g_restart.http = TestHttpProfile();
    g_restart.mdns = TestMdnsRunning();
    g_restart.ap_stops = TestCallCount("StopWifiAccessPoint");
    g_restart.disconnects = TestCallCount("DisconnectWifiStation");
    g_restart.request_calls = (int)RequestFwUpdate_fake.call_count;
}

void Boot(const wifi_credentials_t *stored)
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    TestFakesSetStored(stored);
    g_restart = RestartSnapshot();
    RequestFwUpdate_fake.return_val = FW_UPDATE_REQUEST_SET;
    RestartIntoUpdater_fake.custom_fake = RestartFake;
    TestFakesSetStationAddress(0x0A01A8C0);
    TestFakesSetHostnameInUse("rgb-led-tuner");
    ProvisioningStart();
    Run(0);
}

void StationConnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_CONNECTED); Settle(); }
void StationGotIp() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_GOT_IP); Settle(); }
void StationDisconnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_DISCONNECTED); Settle(); }

void BootIntoPortal()
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(State() == "PORTAL_IDLE");
}

void BootConnected()
{
    Boot(&kStored);
    Run(3000);
    StationConnected();
    REQUIRE(State() == "CONNECTED");
}

/** Same state recipes as test/captive-portal/test_provisioning.cpp and test/tuner-read-measurement. */
void EnterState(const std::string &name)
{
    if (name == "BOOT_WAIT") {
        Boot(nullptr);
    } else if (name == "STA_ATTEMPT") {
        Boot(&kStored);
        Run(1050);
    } else if (name == "STA_PAUSE") {
        Boot(&kStored);
        Run(1050 + 10000);
    } else if (name == "CONNECTED") {
        BootConnected();
    } else if (name == "RECONNECT_WAIT") {
        BootConnected();
        StationDisconnected();
    } else if (name == "RECONNECT_TRY") {
        BootConnected();
        StationDisconnected();
        Run(MockGetNowMs() + 1000);
    } else if (name == "PORTAL_IDLE") {
        BootIntoPortal();
    } else if (name == "PORTAL_TRIAL") {
        BootIntoPortal();
        REQUIRE(TestFakesPortalOps()->submit_credentials(&kNew));
        Settle();
    } else if (name == "PORTAL_SUCCESS") {
        BootIntoPortal();
        REQUIRE(TestFakesPortalOps()->submit_credentials(&kNew));
        Settle();
        StationConnected();
    } else if (name == "PORTAL_RETRY") {
        Boot(nullptr);
        TestFakesFailStart("StartHttpPortal", 1);
        Run(1050);
    }
    REQUIRE(State() == name);
}

button_request_cb_t UpdateCallback()
{
    REQUIRE(StartButton_fake.call_count == 1);
    return StartButton_fake.arg1_val;
}

void Gesture()
{
    UpdateCallback()();
    Settle();
}

const char *const kStates[] = {"BOOT_WAIT",      "STA_ATTEMPT",   "STA_PAUSE",    "CONNECTED",      "RECONNECT_WAIT",
                               "RECONNECT_TRY",  "PORTAL_IDLE",   "PORTAL_TRIAL", "PORTAL_SUCCESS", "PORTAL_RETRY"};

}  // namespace

TEST_CASE("the orchestrator registers an update-gesture callback with the button service", "[T-5][FR-9][FR-10]")
{
    Boot(nullptr);
    REQUIRE(StartButton_fake.call_count == 1);
    CHECK(StartButton_fake.arg0_val != nullptr);
    CHECK(StartButton_fake.arg1_val != nullptr);
    CHECK(StartButton_fake.arg1_val != StartButton_fake.arg0_val);
}

TEST_CASE("MSG_UPDATE_REQUEST in every orchestrator state: request, stop services and Wi-Fi, then restart",
          "[T-5][FR-10]")
{
    const std::string state = GENERATE(from_range(std::begin(kStates), std::end(kStates)));
    CAPTURE(state);
    EnterState(state);
    const int disconnects_before = TestCallCount("DisconnectWifiStation");

    Gesture();

    CHECK(RequestFwUpdate_fake.call_count == 1);
    REQUIRE(g_restart.calls == 1);
    CHECK(g_restart.request_calls == 1);                   /* the request comes first */
    CHECK(g_restart.http == TEST_HTTP_NONE);               /* HTTP server stopped before the restart */
    CHECK_FALSE(g_restart.mdns);
    CHECK(g_restart.ap_stops >= 1);                        /* SoftAP stopped */
    CHECK(g_restart.disconnects == disconnects_before + 1); /* station disconnected */
    CHECK(TestProfileOverlapCount() == 0);
    CHECK(MarkFirmwareHealthy_fake.call_count == 0);
}

TEST_CASE("MSG_UPDATE_REQUEST with an invalid record (NO_RECORD) still restarts", "[T-5][FR-10]")
{
    BootConnected();
    StationGotIp();
    RequestFwUpdate_fake.return_val = FW_UPDATE_REQUEST_NO_RECORD;
    Gesture();
    CHECK(RequestFwUpdate_fake.call_count == 1);
    CHECK(g_restart.calls == 1);
    CHECK(g_restart.http == TEST_HTTP_NONE);
}

TEST_CASE("MSG_UPDATE_REQUEST with a failed write (FAILED) keeps running: no stop, no restart", "[T-5][FR-10]")
{
    const std::string state = GENERATE(std::string("CONNECTED"), std::string("PORTAL_IDLE"));
    CAPTURE(state);
    EnterState(state);
    if (state == "CONNECTED") {
        StationGotIp();
    }
    const test_http_profile_t http_before = TestHttpProfile();
    const bool mdns_before = TestMdnsRunning();
    const int ap_stops = TestCallCount("StopWifiAccessPoint");
    const int disconnects = TestCallCount("DisconnectWifiStation");
    RequestFwUpdate_fake.return_val = FW_UPDATE_REQUEST_FAILED;

    Gesture();

    CHECK(RequestFwUpdate_fake.call_count == 1);
    CHECK(RestartIntoUpdater_fake.call_count == 0);
    CHECK(State() == state);
    CHECK(TestHttpProfile() == http_before);
    CHECK(TestMdnsRunning() == mdns_before);
    CHECK(TestCallCount("StopWifiAccessPoint") == ap_stops);
    CHECK(TestCallCount("DisconnectWifiStation") == disconnects);
    /* The orchestrator keeps working: a later state change is still handled. */
    if (state == "CONNECTED") {
        StationDisconnected();
        CHECK(State() == "RECONNECT_WAIT");
    }
}

TEST_CASE("connected with IP: the station HTTP server and mDNS are stopped before the restart", "[T-5][FR-10]")
{
    BootConnected();
    StationGotIp();
    Run(MockGetNowMs() + 2000);
    REQUIRE(TestHttpProfile() == TEST_HTTP_STATION);
    Gesture();
    CHECK(g_restart.calls == 1);
    CHECK(g_restart.http == TEST_HTTP_NONE);
    CHECK_FALSE(g_restart.mdns);
}

TEST_CASE("healthy deadline: MarkFirmwareHealthy() once at 30,000 ms, never again", "[T-5][FR-11]")
{
    const std::string state = GENERATE(std::string("BOOT_WAIT"), std::string("CONNECTED"), std::string("PORTAL_IDLE"),
                                       std::string("STA_ATTEMPT"));
    CAPTURE(state);
    EnterState(state);
    Run(29999);
    CHECK(MarkFirmwareHealthy_fake.call_count == 0);
    Run(30000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
    Run(200000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
}

TEST_CASE("healthy deadline is independent of message traffic and of the state deadlines", "[T-5][FR-11]")
{
    Boot(&kStored);
    /* Boot attempts with their 10 s deadlines and pauses run through the 30 s mark. */
    for (uint32_t t = 1000; t < 29000; t += 1000) {
        Run(t);
        TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_DISCONNECTED);
        Settle();
    }
    CHECK(MarkFirmwareHealthy_fake.call_count == 0);
    Run(30000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
    Run(100000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
    CHECK(State() != "BOOT_WAIT");   /* the state machine still advanced through its own deadlines */
}

TEST_CASE("the state deadlines are unchanged by the healthy deadline (boot window 1,000 ms, portal entry)",
          "[T-5][FR-31]")
{
    Boot(nullptr);
    Run(999);
    CHECK(State() == "BOOT_WAIT");
    Run(1050);
    CHECK(State() == "PORTAL_IDLE");
}

TEST_CASE("the healthy write and the update request do not write otadata or touch fw_meta from the orchestrator",
          "[T-5][FR-4][FR-8]")
{
    BootConnected();
    Run(30000);
    Gesture();
    /* All flash access goes through fw_update (faked here); the orchestrator itself only calls these three. */
    CHECK(RequestFwUpdate_fake.call_count == 1);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
    CHECK(RestartIntoUpdater_fake.call_count == 1);
}

/* ---- T-19 / FR-11 (sections 0.7, 0.8): healthy clear when Wi-Fi manager initialization fails ------------------------- */

TEST_CASE("Wi-Fi manager init fails -> StartFwHealthyTimer() once, no orchestrator task, no button, no deadline",
          "[T-19][FR-11]")
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    TestFakesSetStored(&kStored);
    TestFakesSetWifiInitOk(false);
    StartFwHealthyTimer_fake.return_val = true;
    ProvisioningStart();
    CHECK(StartFwHealthyTimer_fake.call_count == 1);
    CHECK(MockGetTaskCount() == 0);                 /* SPEC-002: no orchestrator */
    CHECK(StartButton_fake.call_count == 0);
    CHECK(MarkFirmwareHealthy_fake.call_count == 0);  /* the esp_timer callback does it, 30 s later */
    CHECK(std::string(TestLogText()).find("Wi-Fi manager init failed") != std::string::npos);
}

TEST_CASE("Wi-Fi manager init fails and the timer cannot start -> ProvisioningStart still returns cleanly",
          "[T-19][FR-11]")
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    TestFakesSetWifiInitOk(false);
    StartFwHealthyTimer_fake.return_val = false;
    ProvisioningStart();
    CHECK(StartFwHealthyTimer_fake.call_count == 1);
    CHECK(MockGetTaskCount() == 0);
}

TEST_CASE("normal start: the orchestrator deadline clears the count at 30 s and no esp_timer is used",
          "[T-19][FR-11]")
{
    Boot(&kStored);
    CHECK(StartFwHealthyTimer_fake.call_count == 0);
    CHECK(MockGetTaskCount() == 1);
    Run(30000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
    CHECK(StartFwHealthyTimer_fake.call_count == 0);
}

TEST_CASE("the healthy deadline is measured from ProvisioningStart (armed before Wi-Fi init), not from the task start",
          "[T-19][FR-11]")
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    g_restart = RestartSnapshot();
    RestartIntoUpdater_fake.custom_fake = RestartFake;
    ProvisioningStart();   /* at t = 0 */
    MockRunTask(0, 29990);
    CHECK(MarkFirmwareHealthy_fake.call_count == 0);
    MockRunTask(0, 30000);
    CHECK(MarkFirmwareHealthy_fake.call_count == 1);
}
