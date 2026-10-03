/**
 * @file test_mdns_service.cpp
 * @brief SPEC-005 T-6 (FR-17..FR-21, NFR-5 host part, NFR-13): mdns_service.c against FFF fakes of the mDNS API.
 *
 * mocks/mdns.h replaces the managed component's header (NFR-11); mocks/mdns_fakes.c holds the FFF fakes with a call
 * trace and deep copies of the arguments. mdns_service.c is compiled through mocks/mdns_service_harness.c so its
 * running flag can be reset per case. Logging uses the FFF LogWrite() fake of test/ws2812-tuner-page.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "log_fakes.h"
#include "logging.h"
#include "mdns_fakes.h"
#include "mdns_service.h"
void HarnessResetMdnsService(void);
bool HarnessIsMdnsRunning(void);
}

namespace {

void Reset()
{
    TestMdnsReset();
    TestLogReset();
    HarnessResetMdnsService();
}

std::string Log() { return TestLogText(); }
std::string Trace() { return TestMdnsTrace(); }

/** A caller buffer pre-filled with a sentinel, to detect writes on failure. */
struct NameBuffer {
    char text[MDNS_SERVICE_HOSTNAME_MAX];
    NameBuffer() { std::memset(text, '#', sizeof(text)); text[sizeof(text) - 1] = '\0'; }
    bool IsUntouched() const
    {
        for (size_t index = 0; index + 1 < sizeof(text); ++index) {
            if (text[index] != '#') return false;
        }
        return text[sizeof(text) - 1] == '\0';
    }
};

}  // namespace

// ---- Constants (section 7.1) ----------------------------------------------------------------------------------------

TEST_CASE("the mDNS constants have the section 7.1 values", "[T-6][FR-18][FR-21]")
{
    REQUIRE(std::string(MDNS_SERVICE_HOSTNAME) == "rgb-tuner");
    REQUIRE(std::string(MDNS_SERVICE_INSTANCE) == "RGB LED Tuner");
    REQUIRE(MDNS_SERVICE_PORT == 80);
    REQUIRE(MDNS_SERVICE_HOSTNAME_MAX == 64);
}

// ---- StartMdnsService() (FR-18) -------------------------------------------------------------------------------------

TEST_CASE("start initializes, names the host and instance, and adds _http._tcp on port 80", "[T-6][FR-18]")
{
    Reset();
    REQUIRE(StartMdnsService());

    REQUIRE(Trace() == "init,hostname_set,instance_name_set,service_add");
    REQUIRE(std::string(TestMdnsHostnameSet()) == "rgb-tuner");
    REQUIRE(std::string(TestMdnsInstanceSet()) == "RGB LED Tuner");
    REQUIRE(std::string(TestMdnsServiceInstance()) == "RGB LED Tuner");
    REQUIRE(std::string(TestMdnsServiceType()) == "_http");
    REQUIRE(std::string(TestMdnsServiceProto()) == "_tcp");
    REQUIRE(TestMdnsServicePort() == 80);
    REQUIRE(HarnessIsMdnsRunning());
}

TEST_CASE("the service carries exactly one TXT item, path=/", "[T-6][FR-18][NFR-14]")
{
    Reset();
    REQUIRE(StartMdnsService());
    REQUIRE(TestMdnsTxtCount() == 1);
    REQUIRE(std::string(TestMdnsTxtKey(0)) == "path");
    REQUIRE(std::string(TestMdnsTxtValue(0)) == "/");
}

TEST_CASE("a successful start logs the FR-18 Info line once", "[T-6][FR-18]")
{
    Reset();
    REQUIRE(StartMdnsService());
    REQUIRE(Log() == "[L1 mdns_service] mDNS started: rgb-tuner.local, _http._tcp port 80\n");
}

TEST_CASE("start is idempotent: a second call re-initializes nothing", "[T-6][FR-18]")
{
    Reset();
    REQUIRE(StartMdnsService());
    TestLogReset();
    REQUIRE(StartMdnsService());
    REQUIRE(mdns_init_fake.call_count == 1);
    REQUIRE(mdns_service_add_fake.call_count == 1);
    REQUIRE(Log().empty());
}

TEST_CASE("each failing start step frees the responder, logs the step, and returns false", "[T-6][FR-18]")
{
    struct Step {
        const char *call;
        const char *step;
        const char *trace;
    };
    const Step steps[] = {
        {"mdns_init", "init", "init,free"},
        {"mdns_hostname_set", "hostname", "init,hostname_set,free"},
        {"mdns_instance_name_set", "instance", "init,hostname_set,instance_name_set,free"},
        {"mdns_service_add", "service", "init,hostname_set,instance_name_set,service_add,free"},
    };
    for (const Step &step : steps) {
        INFO(step.call);
        Reset();
        TestMdnsFail(step.call, ESP_ERR_NO_MEM);
        REQUIRE_FALSE(StartMdnsService());
        REQUIRE(Trace() == step.trace);
        REQUIRE(mdns_free_fake.call_count == 1);
        REQUIRE(Log() == "[L3 mdns_service] mDNS start failed (step=" + std::string(step.step) + " err=" +
                             std::to_string(ESP_ERR_NO_MEM) + ")\n");
        REQUIRE_FALSE(HarnessIsMdnsRunning());
    }
}

TEST_CASE("after a failed start a retry initializes again and can succeed", "[T-6][FR-9][FR-18]")
{
    Reset();
    TestMdnsFail("mdns_init", ESP_FAIL);
    REQUIRE_FALSE(StartMdnsService());
    TestMdnsFail("", ESP_OK);
    REQUIRE(StartMdnsService());
    REQUIRE(mdns_init_fake.call_count == 2);
    REQUIRE(HarnessIsMdnsRunning());
}

// ---- StopMdnsService() (FR-19) --------------------------------------------------------------------------------------

TEST_CASE("stop frees the responder once and logs mDNS stopped", "[T-6][FR-19]")
{
    Reset();
    REQUIRE(StartMdnsService());
    TestLogReset();
    StopMdnsService();
    REQUIRE(mdns_free_fake.call_count == 1);
    REQUIRE(Log() == "[L1 mdns_service] mDNS stopped\n");
    REQUIRE_FALSE(HarnessIsMdnsRunning());
}

TEST_CASE("stop is safe when not running: no mdns_free(), no log", "[T-6][FR-19]")
{
    Reset();
    StopMdnsService();
    REQUIRE(mdns_free_fake.call_count == 0);
    REQUIRE(Log().empty());

    REQUIRE(StartMdnsService());
    StopMdnsService();
    TestLogReset();
    StopMdnsService();                                             // second stop
    REQUIRE(mdns_free_fake.call_count == 1);
    REQUIRE(Log().empty());
}

TEST_CASE("repeated start/stop cycles are balanced and leak nothing", "[T-6][FR-19][NFR-5]")
{
    Reset();
    constexpr unsigned kCycles = 20;
    for (unsigned cycle = 1; cycle <= kCycles; ++cycle) {
        INFO("cycle " << cycle);
        REQUIRE(StartMdnsService());
        REQUIRE(StartMdnsService());                               // idempotent within a cycle
        StopMdnsService();
        StopMdnsService();                                         // safe double stop
        REQUIRE(mdns_init_fake.call_count == cycle);
        REQUIRE(mdns_free_fake.call_count == cycle);               // every init matched by exactly one free
        REQUIRE(mdns_service_add_fake.call_count == cycle);
    }
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 2 * kCycles);          // one started + one stopped per cycle
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) + TestLogCount(LOG_LEVEL_ERROR) == 0);
}

// ---- ClassifyMdnsHostname() (FR-17, NFR-13) ------------------------------------------------------------------------

TEST_CASE("ClassifyMdnsHostname: default, renamed and unavailable", "[T-6][FR-17][FR-21][NFR-13]")
{
    REQUIRE(ClassifyMdnsHostname("rgb-tuner") == MDNS_HOSTNAME_DEFAULT);
    REQUIRE(ClassifyMdnsHostname("rgb-tuner-2") == MDNS_HOSTNAME_RENAMED);
    REQUIRE(ClassifyMdnsHostname("RGB-TUNER") == MDNS_HOSTNAME_RENAMED);   // exact comparison
    REQUIRE(ClassifyMdnsHostname("rgb-tune") == MDNS_HOSTNAME_RENAMED);
    REQUIRE(ClassifyMdnsHostname("") == MDNS_HOSTNAME_UNAVAILABLE);
    REQUIRE(ClassifyMdnsHostname(nullptr) == MDNS_HOSTNAME_UNAVAILABLE);
}

// ---- LogMdnsHostnameInUse() (FR-21) ---------------------------------------------------------------------------------

TEST_CASE("the default name logs Info and is copied out", "[T-6][FR-21]")
{
    Reset();
    REQUIRE(StartMdnsService());
    TestLogReset();
    TestMdnsSetHostnameInUse("rgb-tuner");
    NameBuffer name;
    REQUIRE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
    REQUIRE(std::string(name.text) == "rgb-tuner");
    REQUIRE(Log() == "[L1 mdns_service] mDNS hostname in use: rgb-tuner.local\n");
}

TEST_CASE("a renamed host logs the conflict Warning and returns the name in use", "[T-6][FR-21]")
{
    Reset();
    TestMdnsSetHostnameInUse("rgb-tuner-2");
    NameBuffer name;
    REQUIRE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
    REQUIRE(std::string(name.text) == "rgb-tuner-2");
    REQUIRE(Log() == "[L2 mdns_service] mDNS hostname conflict: using rgb-tuner-2.local instead of rgb-tuner.local\n");
}

TEST_CASE("the longest valid name (63 characters) is accepted", "[T-6][FR-21]")
{
    Reset();
    const std::string longest(MDNS_SERVICE_HOSTNAME_MAX - 1, 'r');
    TestMdnsSetHostnameInUse(longest.c_str());
    NameBuffer name;
    REQUIRE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
    REQUIRE(std::string(name.text) == longest);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);                 // renamed
}

TEST_CASE("a read error, an empty or an over-long name logs 'hostname unavailable' and leaves the buffer", "[T-6][FR-21]")
{
    SECTION("read error") {
        Reset();
        TestMdnsFail("mdns_hostname_get", ESP_ERR_INVALID_STATE);
        NameBuffer name;
        REQUIRE_FALSE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
        REQUIRE(name.IsUntouched());
        REQUIRE(Log() == "[L2 mdns_service] mDNS hostname unavailable (err=" + std::to_string(ESP_ERR_INVALID_STATE) +
                             ")\n");
    }
    SECTION("empty name") {
        Reset();
        TestMdnsSetHostnameInUse("");
        NameBuffer name;
        REQUIRE_FALSE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
        REQUIRE(name.IsUntouched());
        REQUIRE(Log().rfind("[L2 mdns_service] mDNS hostname unavailable (err=", 0) == 0);
        REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    }
    SECTION("over-long name (64 characters, longer than a DNS label)") {
        Reset();
        const std::string too_long(MDNS_SERVICE_HOSTNAME_MAX, 'r');
        TestMdnsSetHostnameInUse(too_long.c_str());
        NameBuffer name;
        REQUIRE_FALSE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
        REQUIRE(name.IsUntouched());
        REQUIRE(Log().rfind("[L2 mdns_service] mDNS hostname unavailable (err=", 0) == 0);
        REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    }
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
}

TEST_CASE("a too small or NULL buffer is refused without reading the name", "[T-6][FR-21]")
{
    Reset();
    char small[MDNS_SERVICE_HOSTNAME_MAX - 1];
    std::memset(small, '#', sizeof(small));
    REQUIRE_FALSE(LogMdnsHostnameInUse(small, sizeof(small)));
    REQUIRE(small[0] == '#');
    REQUIRE_FALSE(LogMdnsHostnameInUse(nullptr, MDNS_SERVICE_HOSTNAME_MAX));
    REQUIRE(mdns_hostname_get_fake.call_count == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 2);
}

TEST_CASE("the hostname log lines stay within the SPEC-001 127-character limit", "[T-6][FR-21]")
{
    Reset();
    TestMdnsSetHostnameInUse(std::string(MDNS_SERVICE_HOSTNAME_MAX - 1, 'r').c_str());
    NameBuffer name;
    REQUIRE(LogMdnsHostnameInUse(name.text, sizeof(name.text)));
    const std::string line = Log();
    const std::string message = line.substr(line.find("] ") + 2, line.size() - line.find("] ") - 3);
    CAPTURE(message.size());
    REQUIRE(message.size() <= 127);
}
