/**
 * @file test_provisioning.cpp
 * @brief Host tests for the provisioning orchestrator (SPEC-002 T-1, T-2, T-9, T-10; FR-3..FR-6, FR-15..FR-18,
 *        FR-20..FR-23, NFR-5, NFR-8, NFR-9).
 *
 * The orchestrator task runs as a coroutine on a fake millisecond clock. All components it uses are FFF fakes, so a
 * scenario such as "an unreachable network for five attempts" runs instantly and to the millisecond.
 */
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
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
size_t HarnessGetMessageSize(void);
size_t HarnessGetLegacyMessageSize(void);
size_t HarnessGetQueueStorageBytes(void);
size_t HarnessGetLedRequestSize(void);
size_t HarnessGetMeasurementSize(void);
size_t HarnessGetCredentialsSize(void);
}

namespace {

std::vector<provisioning_state_t> g_states;

void OnState(provisioning_state_t state)
{
    g_states.push_back(state);
}

wifi_credentials_t MakeCredentials(const char *ssid, const char *password)
{
    wifi_credentials_t credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

const wifi_credentials_t kStored = MakeCredentials("stored-net", "stored-password");
const wifi_credentials_t kNew = MakeCredentials("new-net", "Sup3rSecretPw!");
const uint32_t kApAddress = 0x0104A8C0;

// ---- Simulation helpers ---------------------------------------------------------------------------------------------

void Run(uint32_t until_ms) { MockRunTask(0, until_ms); }
void Settle() { Run(MockGetNowMs()); }   // let the task process anything just posted

void Boot(const wifi_credentials_t *stored)
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    g_states.clear();
    TestFakesSetStored(stored);
    ProvisioningSetStateCallback(OnState);
    ProvisioningStart();
    Run(0);
}

void PressButton() { TestFakesButtonCallback()(); Settle(); }
void StationConnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_CONNECTED); Settle(); }
void StationDisconnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_DISCONNECTED); Settle(); }

bool Submit(const wifi_credentials_t &credentials)
{
    bool accepted = TestFakesPortalOps()->submit_credentials(&credentials);
    Settle();
    return accepted;
}

std::string State() { return HarnessGetStateName(); }
int Calls(const char *name) { return TestCallCount(name); }

/** Boot without stored credentials; the portal is up from t = 1,050 ms. */
void BootIntoPortal()
{
    Boot(nullptr);
    Run(1050);
    REQUIRE(State() == "PORTAL_IDLE");
}

/** Boot with stored credentials and connect at t = 3,000 ms. */
void BootConnected()
{
    Boot(&kStored);
    Run(3000);
    StationConnected();
    REQUIRE(State() == "CONNECTED");
}

}  // namespace

// ---- Start-up wiring ------------------------------------------------------------------------------------------------

TEST_CASE("start-up initializes the components in dependency order", "[T-11][NFR-9]")
{
    Boot(nullptr);

    REQUIRE(Calls("InitCredentialStore") == 1);
    REQUIRE(Calls("InitWifiManager") == 1);
    REQUIRE(Calls("StartButton") == 1);
    REQUIRE(TestCallPosition("InitCredentialStore", 0) < TestCallPosition("InitWifiManager", 0));   // NVS before Wi-Fi
    REQUIRE(TestFakesWifiCallback() != nullptr);
    REQUIRE(TestFakesButtonCallback() != nullptr);
    REQUIRE(MockGetTaskCount() == 1);                                   // one orchestrator task (the button's is faked)
    REQUIRE(std::string(MockGetTaskName(0)) == "provisioning");
}

TEST_CASE("the orchestrator does not start if Wi-Fi cannot be initialized", "[T-2][NFR-8]")
{
    MockFreeRtosReset();
    TestFakesReset();
    TestLogReset();
    HarnessResetProvisioning();
    TestFakesSetWifiInitOk(false);

    ProvisioningStart();

    REQUIRE(MockGetTaskCount() == 0);
    REQUIRE(Calls("StartButton") == 0);
    REQUIRE(TestLogCount(3) >= 1);
}

// ---- Boot decision --------------------------------------------------------------------------------------------------

TEST_CASE("no stored credentials: provisioning mode starts after the button window", "[T-2][FR-5]")
{
    Boot(nullptr);
    Run(1049);
    REQUIRE(Calls("StartWifiAccessPoint") == 0);

    Run(1050);

    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(Calls("StartDnsServer") == 1);
    REQUIRE(TestDnsAddressAt(0) == kApAddress);
    REQUIRE(Calls("StartHttpPortal") == 1);
    REQUIRE(Calls("ConnectWifiStation") == 0);
    REQUIRE(g_states == std::vector<provisioning_state_t>{PROVISIONING_STATE_PORTAL});
}

TEST_CASE("provisioning mode starts the services in order and stops the station first", "[T-2][FR-7][FR-9][FR-23]")
{
    BootIntoPortal();
    REQUIRE(TestCallPosition("DisconnectWifiStation", 0) < TestCallPosition("StartWifiAccessPoint", 0));
    REQUIRE(TestCallPosition("StartWifiAccessPoint", 0) < TestCallPosition("StartDnsServer", 0));
    REQUIRE(TestCallPosition("StartDnsServer", 0) < TestCallPosition("StartHttpPortal", 0));
    REQUIRE(TestFakesPortalOps() != nullptr);
    REQUIRE(TestFakesPortalOps()->scan_networks == ScanWifiNetworks);
}

TEST_CASE("stored credentials: a station connection is attempted after the button window", "[T-2][FR-4]")
{
    Boot(&kStored);
    Run(1049);
    REQUIRE(Calls("ConnectWifiStation") == 0);

    Run(1050);

    REQUIRE(Calls("ConnectWifiStation") == 1);
    REQUIRE(std::string(TestConnectCredentials(0).ssid) == "stored-net");
    REQUIRE(std::string(TestConnectCredentials(0).password) == "stored-password");
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
    REQUIRE(State() == "STA_ATTEMPT");
    REQUIRE(g_states == std::vector<provisioning_state_t>{PROVISIONING_STATE_CONNECTING});

    StationConnected();
    REQUIRE(State() == "CONNECTED");
    REQUIRE(g_states.back() == PROVISIONING_STATE_CONNECTED);
    Run(3600 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 1);
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
}

TEST_CASE("a button request at boot wins over stored credentials", "[T-1][T-2][FR-3]")
{
    Boot(&kStored);
    Run(500);
    PressButton();

    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(State() == "PORTAL_IDLE");
    Run(60 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 0);
}

TEST_CASE("a button request in the last millisecond of the window still wins", "[T-1][T-2][FR-3]")
{
    Boot(&kStored);
    Run(1049);
    PressButton();
    Run(2000);
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(Calls("ConnectWifiStation") == 0);
}

TEST_CASE("a failed secure NVS start-up leads to provisioning mode", "[T-2][NFR-8]")
{
    MockFreeRtosReset();
    TestFakesReset();
    HarnessResetProvisioning();
    TestFakesSetStoreInitOk(false);       // and no credentials can be loaded
    ProvisioningStart();
    Run(1050);

    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(Calls("ConnectWifiStation") == 0);
}

// ---- Five boot attempts ---------------------------------------------------------------------------------------------

TEST_CASE("an unreachable stored network gets exactly five 10 s attempts 2 s apart", "[T-9][FR-6][NFR-5]")
{
    Boot(&kStored);
    Run(59049);

    REQUIRE(Calls("ConnectWifiStation") == 5);
    const uint32_t attempt_ms[] = {1050, 13050, 25050, 37050, 49050};
    for (int index = 0; index < 5; ++index) {
        INFO("attempt " << index + 1);
        REQUIRE(TestCallTimeMs("ConnectWifiStation", index) == attempt_ms[index]);
        if (index < 4) {   // the fifth attempt has not timed out yet at 59,049 ms
            REQUIRE(TestCallTimeMs("DisconnectWifiStation", index) == attempt_ms[index] + 10000);   // each attempt ends at 10 s
        }
    }
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
    REQUIRE(State() == "STA_ATTEMPT");

    Run(59050);   // the fifth attempt times out: provisioning starts with no extra delay
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(TestCallTimeMs("StartWifiAccessPoint", 0) == 59050);
    REQUIRE(TestCallTimeMs("DisconnectWifiStation", 4) == 59050);   // the fifth attempt also lasted 10 s
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(g_states.back() == PROVISIONING_STATE_PORTAL);
}

TEST_CASE("after five failures provisioning mode has no timeout and no background retry", "[T-9][T-2][FR-6][FR-23]")
{
    Boot(&kStored);
    Run(59050);
    REQUIRE(State() == "PORTAL_IDLE");

    Run(59050 + 24u * 3600 * 1000);

    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("ConnectWifiStation") == 5);
    REQUIRE(Calls("StopWifiAccessPoint") == 0);
    // SPEC-005 FR-6: EnterProvisioning() calls StopMdnsService() then StopHttpPortal() first (safe when nothing
    // runs); that is the only stop. Provisioning itself never stops the portal.
    REQUIRE(Calls("StopHttpPortal") == 1);
    REQUIRE(TestCallPosition("StopHttpPortal", 0) < TestCallPosition("StartWifiAccessPoint", 0));
    REQUIRE(Calls("StopDnsServer") == 0);
}

TEST_CASE("station events during a boot attempt do not shorten it", "[T-9][FR-6]")
{
    Boot(&kStored);
    Run(2000);
    StationDisconnected();      // e.g. wrong password reported by the driver
    Run(13049);
    REQUIRE(Calls("ConnectWifiStation") == 1);
    Run(13050);
    REQUIRE(Calls("ConnectWifiStation") == 2);
}

TEST_CASE("a boot attempt that succeeds stops further attempts", "[T-9][FR-4][FR-6]")
{
    Boot(&kStored);
    Run(25050 + 3000);          // third attempt started at 25,050 ms
    REQUIRE(Calls("ConnectWifiStation") == 3);

    StationConnected();

    REQUIRE(State() == "CONNECTED");
    Run(200 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 3);
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
    REQUIRE(g_states == (std::vector<provisioning_state_t>{PROVISIONING_STATE_CONNECTING, PROVISIONING_STATE_CONNECTED}));
}

TEST_CASE("a button request during a boot attempt aborts it", "[T-1][FR-3]")
{
    Boot(&kStored);
    Run(5000);
    const int disconnects = Calls("DisconnectWifiStation");

    PressButton();

    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("DisconnectWifiStation") == disconnects + 1);
    REQUIRE(TestCallPosition("DisconnectWifiStation", disconnects) < TestCallPosition("StartWifiAccessPoint", 0));
    Run(200 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 1);
}

// ---- Provisioning: submissions --------------------------------------------------------------------------------------

TEST_CASE("a submission is tried with the access point still running", "[T-6][FR-15]")
{
    BootIntoPortal();
    Run(2000);

    REQUIRE(Submit(kNew));

    REQUIRE(Calls("ConnectWifiStation") == 1);
    REQUIRE(std::string(TestConnectCredentials(0).ssid) == "new-net");
    REQUIRE(State() == "PORTAL_TRIAL");
    REQUIRE(Calls("StopWifiAccessPoint") == 0);
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
}

TEST_CASE("a second submission is refused while a trial runs", "[T-6][FR-15][FR-22]")
{
    BootIntoPortal();
    Submit(kNew);
    Run(5000);
    REQUIRE_FALSE(TestFakesPortalOps()->submit_credentials(&kNew));
    Settle();
    REQUIRE(Calls("ConnectWifiStation") == 1);
}

TEST_CASE("a failed trial reports failure after 10 s and keeps the portal and old credentials", "[T-6][FR-16][FR-22]")
{
    BootIntoPortal();
    Run(2000);
    Submit(kNew);
    const int disconnects = Calls("DisconnectWifiStation");

    Run(2000 + 9999);
    REQUIRE(Calls("SetHttpPortalStatus") == 0);
    Run(2000 + 10000);

    REQUIRE(Calls("SetHttpPortalStatus") == 1);
    REQUIRE(TestStatusAt(0) == PORTAL_STATUS_FAILED);
    REQUIRE(Calls("DisconnectWifiStation") == disconnects + 1);
    REQUIRE(Calls("ReplaceCredentials") == 0);                      // previous credentials untouched
    REQUIRE(Calls("StopWifiAccessPoint") == 0);                     // the AP is never stopped by a failure
    REQUIRE(Calls("StopHttpPortal") == 1);                          // SPEC-005 FR-6 stop on entry only, none since
    REQUIRE(Calls("StopDnsServer") == 0);
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(g_states == std::vector<provisioning_state_t>{PROVISIONING_STATE_PORTAL});
}

TEST_CASE("the portal accepts another submission after a failure", "[T-6][T-8][FR-16][NFR-7]")
{
    BootIntoPortal();
    for (int round = 0; round < 5; ++round) {
        INFO("round " << round);
        REQUIRE(Submit(kNew));
        Run(MockGetNowMs() + 10000);
        REQUIRE(State() == "PORTAL_IDLE");
    }
    REQUIRE(Calls("ConnectWifiStation") == 5);
    REQUIRE(Calls("StartWifiAccessPoint") == 1);                    // no re-entry, no extra tasks or servers
    REQUIRE(Calls("StartHttpPortal") == 1);
    REQUIRE(Calls("StartDnsServer") == 1);
    REQUIRE(MockGetTaskCount() == 1);
}

TEST_CASE("station events during a trial do not end it early", "[T-6][FR-15]")
{
    BootIntoPortal();
    Submit(kNew);
    StationDisconnected();
    Run(MockGetNowMs() + 5000);
    REQUIRE(State() == "PORTAL_TRIAL");
    REQUIRE(Calls("SetHttpPortalStatus") == 0);
}

TEST_CASE("a successful trial stores the credentials, shows success, then stops the AP after 3 s", "[T-7][FR-17][FR-18][FR-22]")
{
    BootIntoPortal();
    Run(2000);
    Submit(kNew);
    Run(6000);
    const int disconnects = Calls("DisconnectWifiStation");

    StationConnected();                                             // the new network accepts us at t = 6,000 ms

    REQUIRE(Calls("ReplaceCredentials") == 1);
    REQUIRE(std::string(TestReplaceCredentials(0).ssid) == "new-net");
    REQUIRE(std::string(TestReplaceCredentials(0).password) == "Sup3rSecretPw!");
    REQUIRE(TestStatusAt(0) == PORTAL_STATUS_CONNECTED);
    REQUIRE(TestCallPosition("ReplaceCredentials", 0) < TestCallPosition("SetHttpPortalStatus", 0));   // stored first
    REQUIRE(State() == "PORTAL_SUCCESS");

    // SPEC-005 FR-6: the one StopHttpPortal() so far is the one EnterProvisioning() makes before the AP starts.
    REQUIRE(Calls("StopHttpPortal") == 1);
    REQUIRE(TestCallPosition("StopHttpPortal", 0) < TestCallPosition("StartWifiAccessPoint", 0));
    Run(6000 + 2999);
    REQUIRE(Calls("StopHttpPortal") == 1);                          // the page still has to read "Connected"
    REQUIRE(Calls("StopWifiAccessPoint") == 0);

    Run(6000 + 3000);
    REQUIRE(Calls("StopHttpPortal") == 2);                          // FR-6 entry stop + LeaveProvisioning()
    REQUIRE(TestCallTimeMs("StopHttpPortal", 1) == 9000);
    REQUIRE(Calls("StopDnsServer") == 1);
    REQUIRE(Calls("StopWifiAccessPoint") == 1);
    REQUIRE(TestCallTimeMs("StopWifiAccessPoint", 0) == 9000);
    REQUIRE(TestCallPosition("StopHttpPortal", 1) < TestCallPosition("StopDnsServer", 0));
    REQUIRE(TestCallPosition("StopDnsServer", 0) < TestCallPosition("StopWifiAccessPoint", 0));
    REQUIRE(Calls("DisconnectWifiStation") == disconnects);         // the station link is kept
    REQUIRE(State() == "CONNECTED");
    REQUIRE(g_states.back() == PROVISIONING_STATE_CONNECTED);
}

TEST_CASE("a storage failure after connecting is reported and provisioning continues", "[T-7][FR-17][FR-22]")
{
    BootIntoPortal();
    TestFakesSetReplaceOk(false);
    Submit(kNew);
    const int disconnects = Calls("DisconnectWifiStation");

    StationConnected();

    REQUIRE(Calls("ReplaceCredentials") == 1);
    REQUIRE(TestStatusAt(0) == PORTAL_STATUS_FAILED);
    REQUIRE(Calls("DisconnectWifiStation") == disconnects + 1);
    REQUIRE(State() == "PORTAL_IDLE");
    Run(MockGetNowMs() + 60 * 1000);
    REQUIRE(Calls("StopWifiAccessPoint") == 0);
    REQUIRE(State() == "PORTAL_IDLE");
}

TEST_CASE("if the station drops during the 3 s delay the device reconnects afterwards", "[T-7][T-10][FR-18][FR-21]")
{
    BootIntoPortal();
    Submit(kNew);
    Run(6000);
    StationConnected();
    Run(7000);
    StationDisconnected();
    Run(9000);                                                      // AP shut down here
    REQUIRE(Calls("StopWifiAccessPoint") == 1);
    REQUIRE(State() == "RECONNECT_WAIT");
    REQUIRE(g_states.back() == PROVISIONING_STATE_DISCONNECTED);

    Run(9000 + 1000);
    REQUIRE(Calls("ConnectWifiStation") == 2);
    REQUIRE(std::string(TestConnectCredentials(1).ssid) == "new-net");
}

TEST_CASE("a button request while provisioning is ignored", "[T-1][FR-3]")
{
    BootIntoPortal();
    PressButton();
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    REQUIRE(Calls("StartHttpPortal") == 1);
    REQUIRE(Calls("StartDnsServer") == 1);

    Submit(kNew);
    PressButton();                                                  // also ignored during a trial
    REQUIRE(State() == "PORTAL_TRIAL");
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    Run(MockGetNowMs() + 10000);
    REQUIRE(State() == "PORTAL_IDLE");
}

// ---- Provisioning from a running device ----------------------------------------------------------------------------

TEST_CASE("a button request while connected enters provisioning and drops the station first", "[T-1][FR-3][FR-23]")
{
    BootConnected();
    Run(20000);
    const int disconnects = Calls("DisconnectWifiStation");

    PressButton();

    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(Calls("DisconnectWifiStation") == disconnects + 1);
    REQUIRE(TestCallPosition("DisconnectWifiStation", disconnects) < TestCallPosition("StartWifiAccessPoint", 0));
    REQUIRE(g_states.back() == PROVISIONING_STATE_PORTAL);

    StationDisconnected();                                          // the driver reports the dropped link
    Run(MockGetNowMs() + 3600 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 1);                      // no background reconnect in provisioning mode
}

TEST_CASE("provisioning can be entered and left repeatedly without leaking", "[T-8][FR-20][NFR-7]")
{
    BootConnected();
    for (int round = 1; round <= 3; ++round) {
        INFO("round " << round);
        Run(MockGetNowMs() + 1000);
        PressButton();
        REQUIRE(State() == "PORTAL_IDLE");
        REQUIRE(Submit(kNew));
        StationConnected();
        Run(MockGetNowMs() + 3000);
        REQUIRE(State() == "CONNECTED");

        REQUIRE(Calls("StartHttpPortal") == round);
        // Every start is matched by a stop (LeaveProvisioning()); SPEC-005 FR-6 adds one more StopHttpPortal() at the
        // start of each EnterProvisioning(), which stops the (here not running) station server.
        REQUIRE(Calls("StopHttpPortal") == 2 * round);
        REQUIRE(Calls("StartDnsServer") == round);
        REQUIRE(Calls("StopDnsServer") == round);
        REQUIRE(Calls("StartWifiAccessPoint") == round);
        REQUIRE(Calls("StopWifiAccessPoint") == round);
    }
    REQUIRE(MockGetTaskCount() == 1);
}

// ---- Reconnect after a connection loss ------------------------------------------------------------------------------

TEST_CASE("reconnect waits 1, 2, 4 ... 60 s and never gives up", "[T-10][FR-21]")
{
    BootConnected();
    Run(10000);
    const int base = Calls("ConnectWifiStation");           // the boot connection
    REQUIRE(base == 1);

    StationDisconnected();                                   // connection lost at t = 10,000 ms
    REQUIRE(State() == "RECONNECT_WAIT");
    REQUIRE(g_states.back() == PROVISIONING_STATE_DISCONNECTED);

    const uint32_t delays_ms[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    uint32_t event_ms = 10000;
    for (int index = 0; index < 9; ++index) {
        INFO("retry " << index + 1);
        const uint32_t connect_ms = event_ms + delays_ms[index];
        Run(connect_ms - 1);
        REQUIRE(Calls("ConnectWifiStation") == base + index);
        Run(connect_ms);
        REQUIRE(Calls("ConnectWifiStation") == base + index + 1);
        REQUIRE(TestCallTimeMs("ConnectWifiStation", base + index) == connect_ms);
        REQUIRE(std::string(TestConnectCredentials(base + index).ssid) == "stored-net");

        event_ms = connect_ms + 200;                         // the attempt fails 200 ms later
        Run(event_ms);
        StationDisconnected();
    }

    REQUIRE(TestLogCount(2) >= 9);                           // every failure logged at Warning
    Run(event_ms + 24u * 3600 * 1000);                       // and it keeps trying, never provisioning by itself
    REQUIRE(Calls("StartWifiAccessPoint") == 0);
    REQUIRE(Calls("ConnectWifiStation") > base + 9);
}

TEST_CASE("a reconnect attempt that produces no event fails after 10 s", "[T-10][FR-21]")
{
    BootConnected();
    Run(10000);
    StationDisconnected();
    Run(11000);                                              // first retry starts
    const int disconnects = Calls("DisconnectWifiStation");

    Run(11000 + 10000);                                      // no event: the attempt times out

    REQUIRE(Calls("DisconnectWifiStation") == disconnects + 1);
    REQUIRE(TestLogCount(2) >= 1);
    Run(21000 + 2000 - 1);
    REQUIRE(Calls("ConnectWifiStation") == 2);
    Run(21000 + 2000);                                       // second retry after the doubled delay
    REQUIRE(Calls("ConnectWifiStation") == 3);
}

TEST_CASE("a successful reconnect resets the delay to 1 s", "[T-10][FR-21]")
{
    BootConnected();
    Run(10000);
    StationDisconnected();
    uint32_t event_ms = 10000;
    const uint32_t delays_ms[] = {1000, 2000, 4000};
    for (int index = 0; index < 3; ++index) {                // three failed retries
        Run(event_ms + delays_ms[index]);
        event_ms += delays_ms[index] + 100;
        Run(event_ms);
        StationDisconnected();
    }
    Run(event_ms + 8000);                                    // fourth retry succeeds
    StationConnected();
    REQUIRE(State() == "CONNECTED");
    REQUIRE(g_states.back() == PROVISIONING_STATE_CONNECTED);

    Run(500000);
    const int connects = Calls("ConnectWifiStation");
    StationDisconnected();                                   // lost again
    Run(500000 + 999);
    REQUIRE(Calls("ConnectWifiStation") == connects);
    Run(500000 + 1000);
    REQUIRE(Calls("ConnectWifiStation") == connects + 1);
}

TEST_CASE("a button request while waiting to reconnect enters provisioning", "[T-1][FR-3]")
{
    BootConnected();
    Run(10000);
    StationDisconnected();
    REQUIRE(State() == "RECONNECT_WAIT");

    PressButton();

    REQUIRE(State() == "PORTAL_IDLE");
    Run(MockGetNowMs() + 3600 * 1000);
    REQUIRE(Calls("ConnectWifiStation") == 1);
}

// ---- Credentials in logs --------------------------------------------------------------------------------------------

TEST_CASE("the full provisioning flow never logs the password or the SSID above Debug", "[T-7][FR-19][NFR-2][NFR-12]")
{
    BootIntoPortal();
    Submit(kNew);
    StationConnected();
    Run(MockGetNowMs() + 3000);
    StationDisconnected();
    Run(MockGetNowMs() + 5000);

    const std::string log = TestLogText();
    REQUIRE(log.find("Sup3rSecretPw!") == std::string::npos);
    for (size_t at = log.find("new-net"); at != std::string::npos; at = log.find("new-net", at + 1)) {
        const size_t line_start = log.rfind('\n', at);
        const std::string level = log.substr(line_start == std::string::npos ? 0 : line_start + 1, 4);
        REQUIRE(level == "[L0 ");
    }
}

TEST_CASE("mode changes are logged at Info", "[T-11][NFR-12]")
{
    BootIntoPortal();
    REQUIRE(std::string(TestLogText()).find("[L1 provision] entering provisioning mode") != std::string::npos);
}

// ---- Provisioning services that fail to start -----------------------------------------------------------------------

TEST_CASE("a failed portal start is undone and retried after 5 s", "[T-8][FR-20][NFR-7]")
{
    Boot(nullptr);
    TestFakesFailStart("StartHttpPortal", 1);       // the HTTP server cannot start the first time
    Run(1050);

    REQUIRE(State() == "PORTAL_RETRY");
    REQUIRE(TestLogCount(3) >= 1);                                  // Error logged
    // SPEC-005 FR-6 stop on entry, then the cleanup of the partial start.
    REQUIRE(Calls("StopHttpPortal") == 2);
    REQUIRE(TestCallTimeMs("StopHttpPortal", 1) == 1050);
    REQUIRE(Calls("StopDnsServer") == 1);
    REQUIRE(Calls("StopWifiAccessPoint") == 1);
    REQUIRE(g_states.empty());                                      // no "portal" state was announced

    Run(1050 + 4999);
    REQUIRE(Calls("StartWifiAccessPoint") == 1);
    Run(1050 + 5000);

    REQUIRE(Calls("StartWifiAccessPoint") == 2);
    REQUIRE(Calls("StartHttpPortal") == 2);
    REQUIRE(State() == "PORTAL_IDLE");
    REQUIRE(g_states == std::vector<provisioning_state_t>{PROVISIONING_STATE_PORTAL});
}

TEST_CASE("each service that can fail to start triggers the retry", "[T-8][FR-20]")
{
    for (const char *service : {"StartWifiAccessPoint", "StartDnsServer", "StartHttpPortal"}) {
        INFO(service);
        Boot(nullptr);
        TestFakesFailStart(service, 2);
        Run(1050);
        REQUIRE(State() == "PORTAL_RETRY");
        Run(1050 + 5000);
        REQUIRE(State() == "PORTAL_RETRY");                         // fails a second time
        Run(1050 + 10000);
        REQUIRE(State() == "PORTAL_IDLE");                          // and recovers on the third try
        REQUIRE(Calls("StopWifiAccessPoint") == 2);                 // every failed try was undone
    }
}

TEST_CASE("the retry keeps going and never gives up", "[T-8][FR-20]")
{
    Boot(nullptr);
    TestFakesFailStart("StartWifiAccessPoint", 1000);
    Run(1050 + 60 * 1000);
    REQUIRE(State() == "PORTAL_RETRY");
    REQUIRE(Calls("StartWifiAccessPoint") >= 12);
    REQUIRE(Calls("StartDnsServer") == 0);                          // short-circuit: later services are not started
}

TEST_CASE("while the portal is retrying, button requests and submissions are ignored", "[T-1][FR-3]")
{
    Boot(nullptr);
    TestFakesFailStart("StartDnsServer", 1);
    Run(1050);
    REQUIRE(State() == "PORTAL_RETRY");

    PressButton();
    REQUIRE(Calls("StartWifiAccessPoint") == 1);                    // no extra attempt, the timer decides
    REQUIRE(TestFakesPortalOps() == nullptr);                       // the HTTP server never started
}

TEST_CASE("the state lock is always released", "[T-8][NFR-9]")
{
    BootIntoPortal();
    Submit(kNew);
    StationConnected();
    Run(MockGetNowMs() + 3000);
    REQUIRE(MockGetMutexBalance() == 0);
}

// ---- SPEC-004 T-5 (FR-5, FR-6, NFR-5): tuner timing hand-off to led_controller ---------------------------------------

namespace {

const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};
// SPEC-003 vector X (maximum edge, valid under V4). Vector D (inverted highs, equal periods) was used here before
// 2026-10-03; it now fails V4 and can no longer reach the orchestrator through /tuner (SPEC-004 FR-27 note).
const ws2812_timing_t kVectorX = {1000, 2000, 1200, 2000, 800};

bool SameTiming(const ws2812_timing_t &a, const ws2812_timing_t &b)
{
    return a.bit0_high_ns == b.bit0_high_ns && a.bit0_period_ns == b.bit0_period_ns &&
           a.bit1_high_ns == b.bit1_high_ns && a.bit1_period_ns == b.bit1_period_ns && a.reset_us == b.reset_us;
}

/** Hand one timing set to the orchestrator exactly as http_portal does, then let the orchestrator run. */
void SubmitLedTiming(const ws2812_timing_t &timing, uint32_t submit_seq = 1)
{
    HarnessGetPortalOps()->apply_led_timing(&timing, submit_seq);
    Settle();
}

/** Bring the orchestrator into the named state, using only the component fakes and the fake clock. */
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
        REQUIRE(Submit(kNew));
    } else if (name == "PORTAL_SUCCESS") {
        BootIntoPortal();
        REQUIRE(Submit(kNew));
        StationConnected();
    } else if (name == "PORTAL_RETRY") {
        Boot(nullptr);
        TestFakesFailStart("StartHttpPortal", 1);
        Run(1050);
    }
    REQUIRE(State() == name);
}

}  // namespace

TEST_CASE("the portal ops table wires apply_led_timing", "[SPEC-004][T-5][FR-4][FR-5]")
{
    BootIntoPortal();
    REQUIRE(TestFakesPortalOps() == HarnessGetPortalOps());   // the table StartHttpPortal() received
    REQUIRE(TestFakesPortalOps()->apply_led_timing != nullptr);
    REQUIRE(TestFakesPortalOps()->submit_credentials != nullptr);
    REQUIRE(TestFakesPortalOps()->scan_networks != nullptr);
}

TEST_CASE("apply_led_timing only posts a message; ApplyWs2812Timing runs on the orchestrator task", "[SPEC-004][T-5][FR-5]")
{
    BootIntoPortal();
    HarnessGetPortalOps()->apply_led_timing(&kVectorA, 1);

    REQUIRE(ApplyWs2812Timing_fake.call_count == 0);   // returned without driving anything (non-blocking hop 1)

    Settle();
    REQUIRE(ApplyWs2812Timing_fake.call_count == 1);
    REQUIRE(SameTiming(TestAppliedLedTiming(0), kVectorA));
}

TEST_CASE("MSG_LED_TIMING_SUBMITTED calls ApplyWs2812Timing once in every orchestrator state", "[SPEC-004][T-5][FR-6]")
{
    const char *const kStates[] = {
        "BOOT_WAIT", "STA_ATTEMPT", "STA_PAUSE", "CONNECTED", "RECONNECT_WAIT",
        "RECONNECT_TRY", "PORTAL_IDLE", "PORTAL_TRIAL", "PORTAL_SUCCESS", "PORTAL_RETRY",
    };
    for (const char *name : kStates) {
        DYNAMIC_SECTION("state " << name)
        {
            EnterState(name);
            const int connects_before = Calls("ConnectWifiStation");
            const int statuses_before = Calls("SetHttpPortalStatus");

            SubmitLedTiming(kVectorX);

            REQUIRE(ApplyWs2812Timing_fake.call_count == 1);
            REQUIRE(SameTiming(TestAppliedLedTiming(0), kVectorX));
            REQUIRE(State() == name);                                   // no state transition
            REQUIRE(Calls("ConnectWifiStation") == connects_before);    // no Wi-Fi side effect
            REQUIRE(Calls("SetHttpPortalStatus") == statuses_before);
        }
    }
}

TEST_CASE("each timing message yields its own ApplyWs2812Timing call, in order", "[SPEC-004][T-5][FR-6]")
{
    BootIntoPortal();
    HarnessGetPortalOps()->apply_led_timing(&kVectorA, 1);
    HarnessGetPortalOps()->apply_led_timing(&kVectorX, 2);
    Settle();

    REQUIRE(ApplyWs2812Timing_fake.call_count == 2);
    REQUIRE(SameTiming(TestAppliedLedTiming(0), kVectorA));
    REQUIRE(SameTiming(TestAppliedLedTiming(1), kVectorX));
}

TEST_CASE("a timing submission during a credential trial does not disturb the trial", "[SPEC-004][T-5][FR-6]")
{
    EnterState("PORTAL_TRIAL");
    SubmitLedTiming(kVectorA);
    REQUIRE(ApplyWs2812Timing_fake.call_count == 1);

    StationConnected();   // the trial still completes normally
    REQUIRE(State() == "PORTAL_SUCCESS");
    REQUIRE(Calls("ReplaceCredentials") == 1);
}

TEST_CASE("a full orchestrator queue drops the timing message with a Warning", "[SPEC-004][T-5][FR-5]")
{
    BootIntoPortal();
    TestLogReset();
    for (int index = 0; index < 8; ++index) {   // QUEUE_LENGTH = 8, the orchestrator has not run yet
        HarnessGetPortalOps()->apply_led_timing(&kVectorA, 1);
    }
    REQUIRE(TestLogCount(2) == 0);

    HarnessGetPortalOps()->apply_led_timing(&kVectorX, 2);   // ninth: dropped, never blocks
    REQUIRE(TestLogCount(2) == 1);
    REQUIRE(std::string(TestLogText()).find("orchestrator queue full") != std::string::npos);

    Settle();
    REQUIRE(ApplyWs2812Timing_fake.call_count == 8);
    REQUIRE(SameTiming(TestAppliedLedTiming(7), kVectorA));
}

TEST_CASE("apply_led_timing(NULL) posts nothing: no message, no queue slot, no Warning", "[SPEC-004][T-5][FR-5]")
{
    BootIntoPortal();
    TestLogReset();
    HarnessGetPortalOps()->apply_led_timing(nullptr, 1);
    REQUIRE(TestLogCount(2) == 0);

    // The NULL call used no queue slot: eight valid submissions still all fit (QUEUE_LENGTH = 8).
    for (int index = 0; index < 8; ++index) {
        HarnessGetPortalOps()->apply_led_timing(&kVectorA, 1);
    }
    REQUIRE(TestLogCount(2) == 0);
    REQUIRE(std::string(TestLogText()).find("orchestrator queue full") == std::string::npos);

    Settle();
    REQUIRE(ApplyWs2812Timing_fake.call_count == 8);   // only the eight valid sets, never an all-zero one
    for (int index = 0; index < 8; ++index) {
        REQUIRE(SameTiming(TestAppliedLedTiming(index), kVectorA));
    }
}

TEST_CASE("apply_led_timing(NULL) alone never reaches ApplyWs2812Timing", "[SPEC-004][T-5][FR-5]")
{
    BootIntoPortal();
    HarnessGetPortalOps()->apply_led_timing(nullptr, 1);
    Settle();
    REQUIRE(ApplyWs2812Timing_fake.call_count == 0);
    REQUIRE(State() == "PORTAL_IDLE");
}

TEST_CASE("message_t is a union: adding the timing payload does not grow the queue storage", "[SPEC-004][T-5][NFR-5]")
{
    REQUIRE(sizeof(ws2812_timing_t) == 10);
    REQUIRE(HarnessGetMessageSize() == HarnessGetLegacyMessageSize());
    REQUIRE(HarnessGetQueueStorageBytes() == 8 * HarnessGetLegacyMessageSize());
}


// ---- SPEC-004 2026-10-03 (FR-5/FR-6 submit_seq, FR-34, NFR-5): sequence pass-through and MSG_PULSE_RESULT ----------

namespace {

ws2812_measurement_t Measurement(uint32_t submit_seq, ws2812_measurement_state_t state, uint32_t b0 = 0,
                                 uint32_t b1 = 0, uint16_t match = 0, bool available = false)
{
    ws2812_measurement_t measurement = {};
    measurement.submit_seq = submit_seq;
    measurement.state = state;
    measurement.bit0_high_avg_ns = b0;
    measurement.bit1_high_avg_ns = b1;
    measurement.match_count = match;
    measurement.match_available = available;
    return measurement;
}

bool SameMeasurement(const ws2812_measurement_t &a, const ws2812_measurement_t &b)
{
    return a.submit_seq == b.submit_seq && a.state == b.state && a.bit0_high_avg_ns == b.bit0_high_avg_ns &&
           a.bit1_high_avg_ns == b.bit1_high_avg_ns && a.match_count == b.match_count &&
           a.match_available == b.match_available;
}

}  // namespace

TEST_CASE("the submit_seq passes unchanged from apply_led_timing to ApplyWs2812Timing", "[SPEC-004][T-5][FR-5][FR-6][SPEC-003][T-16][FR-23]")
{
    BootIntoPortal();
    for (uint32_t seq : {1u, 2u, 77u, 4294967295u}) {
        HarnessGetPortalOps()->apply_led_timing(&kVectorA, seq);
    }
    Settle();
    REQUIRE(ApplyWs2812Timing_fake.call_count == 4);
    REQUIRE(TestAppliedSubmitSeq(0) == 1);
    REQUIRE(TestAppliedSubmitSeq(1) == 2);
    REQUIRE(TestAppliedSubmitSeq(2) == 77);
    REQUIRE(TestAppliedSubmitSeq(3) == 4294967295u);   // full 32 bits, no truncation in led_request_t
    REQUIRE(ApplyWs2812Timing_fake.arg1_history[3] == 4294967295u);
}

TEST_CASE("ProvisioningStart registers the pulse-result callback after the queue exists and before the task", "[SPEC-004][T-5][FR-34]")
{
    Boot(nullptr);
    REQUIRE(SetPulseResultCallback_fake.call_count == 1);
    REQUIRE(TestPulseResultCallback() != nullptr);
    REQUIRE(TestPulseCallbackSawQueue());              // the queue was created first
    REQUIRE(TestPulseCallbackTaskCount() == 0);        // the orchestrator task did not exist yet
    REQUIRE(MockGetTaskCount() == 1);
}

TEST_CASE("the pulse-result callback posts MSG_PULSE_RESULT with a 0 timeout and returns", "[SPEC-004][T-5][FR-34][FR-37]")
{
    BootIntoPortal();
    const int sends_before = MockGetQueueSendCount();
    const ws2812_measurement_t done = Measurement(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    TestPulseResultCallback()(&done);

    REQUIRE(MockGetQueueSendCount() == sends_before + 1);
    REQUIRE(MockGetLastQueueSendWait() == 0);          // never blocks the decode or driver task
    REQUIRE(SetHttpTunerResult_fake.call_count == 0);  // stored only by the orchestrator task
    Settle();
    REQUIRE(SetHttpTunerResult_fake.call_count == 1);
    REQUIRE(SameMeasurement(TestTunerResultAt(0), done));
}

TEST_CASE("MSG_PULSE_RESULT calls SetHttpTunerResult once in every orchestrator state", "[SPEC-004][T-5][FR-34][SPEC-003][FR-24]")
{
    const char *const kStates[] = {
        "BOOT_WAIT", "STA_ATTEMPT", "STA_PAUSE", "CONNECTED", "RECONNECT_WAIT",
        "RECONNECT_TRY", "PORTAL_IDLE", "PORTAL_TRIAL", "PORTAL_SUCCESS", "PORTAL_RETRY",
    };
    for (const char *name : kStates) {
        DYNAMIC_SECTION("state " << name)
        {
            EnterState(name);
            const int connects_before = Calls("ConnectWifiStation");
            const int statuses_before = Calls("SetHttpPortalStatus");
            const ws2812_measurement_t timeout = Measurement(9, WS2812_MEASUREMENT_TIMEOUT);

            TestPulseResultCallback()(&timeout);
            Settle();

            REQUIRE(SetHttpTunerResult_fake.call_count == 1);
            REQUIRE(SameMeasurement(TestTunerResultAt(0), timeout));
            REQUIRE(State() == name);
            REQUIRE(Calls("ConnectWifiStation") == connects_before);
            REQUIRE(Calls("SetHttpPortalStatus") == statuses_before);
            REQUIRE(ApplyWs2812Timing_fake.call_count == 0);
        }
    }
}

TEST_CASE("every measurement state is forwarded field by field, in order", "[SPEC-004][T-5][FR-34][FR-35][FR-36]")
{
    BootIntoPortal();
    const ws2812_measurement_t results[] = {
        Measurement(1, WS2812_MEASUREMENT_DONE, 500, 500, 0, false),
        Measurement(2, WS2812_MEASUREMENT_COUNT_ERROR),
        Measurement(3, WS2812_MEASUREMENT_NOT_MEASURED),
        Measurement(4, WS2812_MEASUREMENT_TIMEOUT),
    };
    for (const ws2812_measurement_t &result : results) {
        TestPulseResultCallback()(&result);
    }
    TestPulseResultCallback()(nullptr);                // NULL: nothing is posted
    Settle();
    REQUIRE(SetHttpTunerResult_fake.call_count == 4);
    for (int index = 0; index < 4; ++index) {
        REQUIRE(SameMeasurement(TestTunerResultAt(index), results[index]));
    }
}

TEST_CASE("a full orchestrator queue drops the pulse result with a Warning", "[SPEC-004][T-5][FR-34]")
{
    BootIntoPortal();
    TestLogReset();
    const ws2812_measurement_t result = Measurement(5, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    for (int index = 0; index < 8; ++index) {          // QUEUE_LENGTH = 8, the orchestrator has not run yet
        TestPulseResultCallback()(&result);
    }
    REQUIRE(TestLogCount(2) == 0);
    TestPulseResultCallback()(&result);                // ninth: dropped, never blocks
    REQUIRE(TestLogCount(2) == 1);
    REQUIRE(std::string(TestLogText()).find("orchestrator queue full, message 6 dropped") != std::string::npos);
    REQUIRE(MockGetLastQueueSendWait() == 0);
    Settle();
    REQUIRE(SetHttpTunerResult_fake.call_count == 8);
}

TEST_CASE("message_t stays the size of the SPEC-002 layout with the led_request and measurement members", "[SPEC-004][T-5][NFR-5]")
{
    REQUIRE(HarnessGetLedRequestSize() == 16);         // ws2812_timing_t (10) + padding + submit_seq
    REQUIRE(HarnessGetMeasurementSize() == 20);        // SPEC-003 section 7.6
    REQUIRE(HarnessGetLedRequestSize() <= HarnessGetCredentialsSize());
    REQUIRE(HarnessGetMeasurementSize() <= HarnessGetCredentialsSize());
    REQUIRE(HarnessGetMessageSize() == HarnessGetLegacyMessageSize());
    REQUIRE(HarnessGetQueueStorageBytes() == 8 * HarnessGetLegacyMessageSize());
}
