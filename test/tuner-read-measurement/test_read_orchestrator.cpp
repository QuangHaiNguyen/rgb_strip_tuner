/**
 * @file test_read_orchestrator.cpp
 * @brief SPEC-006 T-4, orchestrator part (FR-9, FR-10, NFR-3, NFR-7): http_portal_ops_t::request_pulse_read posts
 *        MSG_PULSE_READ_REQUESTED without blocking; HandleMessage() calls ArmPulseRead(seq) in every orchestrator state
 *        and never ApplyWs2812Timing(); a full queue drops the request with a Warning; sizeof(message_t) unchanged.
 *
 * provisioning.c runs as a coroutine on the fake millisecond clock of test/captive-portal/mocks/freertos_mock.c, with
 * every component an FFF fake (test/captive-portal/mocks/provisioning_fakes.c, which has the ArmPulseRead() fake).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

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
size_t HarnessGetCredentialsSize(void);
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
const ws2812_timing_t kVectorA = {400, 1250, 800, 1250, 280};

void Run(uint32_t until_ms) { MockRunTask(0, until_ms); }
void Settle() { Run(MockGetNowMs()); }

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

void StationConnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_CONNECTED); Settle(); }
void StationDisconnected() { TestFakesWifiCallback()(WIFI_MANAGER_EVENT_STA_DISCONNECTED); Settle(); }
std::string State() { return HarnessGetStateName(); }
int Calls(const char *name) { return TestCallCount(name); }

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

/** Same state recipes as test/captive-portal/test_provisioning.cpp (SPEC-004 T-5). */
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

}  // namespace

TEST_CASE("the ops table wires request_pulse_read (portal and station profiles get the same table)", "[T-4][FR-10]")
{
    BootIntoPortal();
    REQUIRE(TestFakesPortalOps() == HarnessGetPortalOps());
    REQUIRE(HarnessGetPortalOps()->request_pulse_read != nullptr);
    REQUIRE(HarnessGetPortalOps()->apply_led_timing != nullptr);
}

TEST_CASE("request_pulse_read only posts a message with a 0 timeout; ArmPulseRead runs on the orchestrator task", "[T-4][FR-10][NFR-3][NFR-7]")
{
    BootIntoPortal();
    const int sends_before = MockGetQueueSendCount();
    HarnessGetPortalOps()->request_pulse_read(5);

    REQUIRE(MockGetQueueSendCount() == sends_before + 1);
    REQUIRE(MockGetLastQueueSendWait() == 0);         // never blocks the HTTP server task
    REQUIRE(ArmPulseRead_fake.call_count == 0);       // nothing armed from the HTTP task
    REQUIRE(TestLogCount(2) == 0);

    Settle();
    REQUIRE(ArmPulseRead_fake.call_count == 1);
    REQUIRE(ArmPulseRead_fake.arg0_val == 5);
}

TEST_CASE("MSG_PULSE_READ_REQUESTED calls ArmPulseRead once in every orchestrator state, never ApplyWs2812Timing", "[T-4][FR-9][FR-10]")
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
            const int replaces_before = Calls("ReplaceCredentials");
            const int loads_before = Calls("LoadCredentials");
            const int portal_starts_before = Calls("StartHttpPortal");
            const int station_starts_before = static_cast<int>(StartHttpStationServer_fake.call_count);
            TestLogReset();

            HarnessGetPortalOps()->request_pulse_read(42);
            Settle();

            REQUIRE(ArmPulseRead_fake.call_count == 1);
            REQUIRE(ArmPulseRead_fake.arg0_val == 42);
            REQUIRE(ApplyWs2812Timing_fake.call_count == 0);      // FR-9: no frame generated
            REQUIRE(SetHttpTunerResult_fake.call_count == 0);     // nothing published by the orchestrator itself
            REQUIRE(State() == name);                             // no state transition
            REQUIRE(Calls("ConnectWifiStation") == connects_before);
            REQUIRE(Calls("SetHttpPortalStatus") == statuses_before);
            REQUIRE(Calls("ReplaceCredentials") == replaces_before);   // no NVS access
            REQUIRE(Calls("LoadCredentials") == loads_before);
            REQUIRE(Calls("StartHttpPortal") == portal_starts_before);
            REQUIRE(static_cast<int>(StartHttpStationServer_fake.call_count) == station_starts_before);
            REQUIRE(TestLogCount(2) == 0);
            REQUIRE(TestLogCount(3) == 0);
        }
    }
}

TEST_CASE("the submit_seq passes unchanged, full 32 bits, in order", "[T-4][FR-10]")
{
    BootIntoPortal();
    for (uint32_t seq : {1u, 2u, 77u, 4294967295u}) {
        HarnessGetPortalOps()->request_pulse_read(seq);
    }
    Settle();
    REQUIRE(ArmPulseRead_fake.call_count == 4);
    REQUIRE(ArmPulseRead_fake.arg0_history[0] == 1);
    REQUIRE(ArmPulseRead_fake.arg0_history[1] == 2);
    REQUIRE(ArmPulseRead_fake.arg0_history[2] == 77);
    REQUIRE(ArmPulseRead_fake.arg0_history[3] == 4294967295u);
}

TEST_CASE("interleaved Send and Read messages keep their own paths and numbers", "[T-4][FR-9][FR-10]")
{
    BootIntoPortal();
    HarnessGetPortalOps()->request_pulse_read(1);
    HarnessGetPortalOps()->apply_led_timing(&kVectorA, 2);
    HarnessGetPortalOps()->request_pulse_read(3);
    Settle();
    REQUIRE(ArmPulseRead_fake.call_count == 2);
    REQUIRE(ArmPulseRead_fake.arg0_history[0] == 1);
    REQUIRE(ArmPulseRead_fake.arg0_history[1] == 3);
    REQUIRE(ApplyWs2812Timing_fake.call_count == 1);
    REQUIRE(ApplyWs2812Timing_fake.arg1_val == 2);
}

TEST_CASE("a full orchestrator queue drops the read request with a Warning and never blocks", "[T-4][FR-10][NFR-3]")
{
    BootIntoPortal();
    TestLogReset();
    for (uint32_t seq = 1; seq <= 8; ++seq) {   // QUEUE_LENGTH = 8, the orchestrator has not run yet
        HarnessGetPortalOps()->request_pulse_read(seq);
    }
    REQUIRE(TestLogCount(2) == 0);

    HarnessGetPortalOps()->request_pulse_read(9);   // ninth: dropped
    REQUIRE(TestLogCount(2) == 1);
    REQUIRE(std::string(TestLogText()).find("orchestrator queue full, message 7 dropped") != std::string::npos);
    REQUIRE(MockGetLastQueueSendWait() == 0);

    Settle();
    REQUIRE(ArmPulseRead_fake.call_count == 8);     // the dropped request arms nothing and publishes nothing
    REQUIRE(ArmPulseRead_fake.arg0_history[7] == 8);
    REQUIRE(SetHttpTunerResult_fake.call_count == 0);
}

TEST_CASE("a read during a credential trial does not disturb the trial", "[T-4][FR-9][FR-10]")
{
    EnterState("PORTAL_TRIAL");
    HarnessGetPortalOps()->request_pulse_read(4);
    Settle();
    REQUIRE(ArmPulseRead_fake.call_count == 1);
    StationConnected();
    REQUIRE(State() == "PORTAL_SUCCESS");
    REQUIRE(Calls("ReplaceCredentials") == 1);
}

TEST_CASE("sizeof(message_t) does not grow with the submit_seq member", "[T-4][FR-10][NFR-2]")
{
    REQUIRE(sizeof(uint32_t) <= HarnessGetCredentialsSize());
    REQUIRE(HarnessGetMessageSize() == HarnessGetLegacyMessageSize());
    REQUIRE(HarnessGetQueueStorageBytes() == 8 * HarnessGetLegacyMessageSize());
}
