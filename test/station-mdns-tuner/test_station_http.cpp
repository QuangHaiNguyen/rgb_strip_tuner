/**
 * @file test_station_http.cpp
 * @brief SPEC-005 T-3, T-4, T-20: the station profile of http_portal through the httpd simulator
 *        (FR-7, FR-11..FR-14, FR-16, FR-27..FR-30, NFR-9, NFR-14, NFR-16).
 *
 * http_portal.c is compiled through test/captive-portal/mocks/http_portal_harness.c so its statics (server handle,
 * profile, station identity and its mutex) can be reset per case. Logging goes through the FFF LogWrite() fake of
 * test/ws2812-tuner-page; http_portal_ops_t::apply_led_timing is an FFF fake. Request headers (Origin, Host) are
 * served by the simulator's httpd_req_get_hdr_value_str().
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fff.h"   // FFF globals live in test/ws2812-tuner-page/mocks/log_fakes.c

extern "C" {
#include "freertos_mock.h"
#include "http_portal.h"
#include "httpd_mock.h"
#include "log_fakes.h"
#include "logging.h"
#include "mdns_service.h"
#include "nvs_fake.h"
#include "tuner_page.h"
#include "ws2812_timing.h"
void HarnessResetHttpPortal(void);
bool HarnessHasIdentityMutex(void);
int HarnessGetIdentityMutexTakes(void);
int HarnessGetIdentityMutexGives(void);
const char *HarnessGetIdentityName(void);
uint32_t HarnessGetIdentityAddress(void);
}

namespace {

constexpr int kHttpPut = 4;   // http_parser method number; the station profile registers no PUT handler

uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t octets[4] = {a, b, c, d};
    uint32_t address = 0;
    std::memcpy(&address, octets, sizeof(address));
    return address;
}

const uint32_t kStationIp = Ipv4(192, 168, 1, 42);
const char *const kVectorA = "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280";
const char *const kVectorALog =
    "tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280";
const char *const kRejectLine = "[L2 http_portal] tuner request rejected: reason=foreign_origin\n";

int g_scan_calls = 0;
int g_submit_calls = 0;

int FakeScan(wifi_scan_entry_t *, uint16_t)
{
    ++g_scan_calls;
    return 0;
}

bool FakeSubmit(const wifi_credentials_t *)
{
    ++g_submit_calls;
    return true;
}

FAKE_VOID_FUNC(FakeApplyLedTiming, const ws2812_timing_t *);

const http_portal_ops_t kOps = {FakeScan, FakeSubmit, FakeApplyLedTiming};

void ResetAll()
{
    MockFreeRtosReset();
    TestHttpdReset();
    TestNvsReset();
    HarnessResetHttpPortal();
    TestLogReset();
    g_scan_calls = 0;
    g_submit_calls = 0;
    RESET_FAKE(FakeApplyLedTiming);
}

/** Station profile with the section 7.5 identity (renamed host rgb-tuner-2, 192.168.1.42). */
void StartStation(const char *hostname = "rgb-tuner-2", uint32_t station_ipv4 = kStationIp)
{
    ResetAll();
    SetHttpStationIdentity(hostname, station_ipv4);
    REQUIRE(StartHttpStationServer(&kOps));
    TestLogReset();   // discard "station HTTP server started"
}

void StartProvisioning()
{
    ResetAll();
    REQUIRE(StartHttpPortal(&kOps));
    TestLogReset();
}

std::string Status() { return TestHttpdStatus(); }
std::string Body() { return TestHttpdBody(); }
std::string Log() { return TestLogText(); }
std::string Header(const char *name)
{
    const char *value = TestHttpdHeader(name);
    return value == nullptr ? std::string("<none>") : std::string(value);
}

int g_recv_before = 0;   // httpd_req_recv() count before the last request

esp_err_t Request(int method, const char *uri, const char *body)
{
    g_recv_before = TestHttpdRecvCount();
    return TestHttpdRequest(method, uri, body);
}

esp_err_t PostTuner(const char *origin, const char *body = kVectorA)
{
    TestHttpdSetRequestHeader("Origin", origin);   // NULL removes the header
    return Request(HTTP_POST, "/tuner", body);
}

bool HasUri(const char *uri, int method)
{
    for (int index = 0; index < TestHttpdHandlerCount(); ++index) {
        if (std::string(TestHttpdUriAt(index)) == uri && TestHttpdMethodAt(index) == method) {
            return true;
        }
    }
    return false;
}

const char *const kProbeUris[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
    "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/check_network_status.txt",
};

void RequireForeignRejection(const std::string &origin_text)
{
    REQUIRE(Status() == "403 Forbidden");
    REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(Header("Cache-Control") == "no-store");
    REQUIRE(Body() == "Forbidden origin");
    REQUIRE(Header("Location") == "<none>");
    REQUIRE(Log() == kRejectLine);                                  // exactly one line: the Warning
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
    REQUIRE(TestHttpdRecvCount() == g_recv_before);                 // the body was never read
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
    if (!origin_text.empty()) {
        REQUIRE(std::string(TestHttpdAllOutput()).find(origin_text) == std::string::npos);
        REQUIRE(Log().find(origin_text) == std::string::npos);
    }
}

void RequireVectorAAccepted()
{
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Sent");
    REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(Header("Cache-Control") == "no-store");
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);
    REQUIRE(Log().find(kVectorALog) != std::string::npos);
    REQUIRE(FakeApplyLedTiming_fake.call_count == 1);
    REQUIRE(Log().find("foreign_origin") == std::string::npos);
}

}  // namespace

// ==== T-3: registration set, configuration, single instance =========================================================

TEST_CASE("the station profile registers exactly GET /, GET /tuner and POST /tuner", "[T-3][FR-11]")
{
    ResetAll();
    REQUIRE(StartHttpStationServer(&kOps));

    REQUIRE(TestHttpdHandlerCount() == 3);
    REQUIRE(HasUri("/", HTTP_GET));
    REQUIRE(HasUri("/tuner", HTTP_GET));
    REQUIRE(HasUri("/tuner", HTTP_POST));
    REQUIRE(Log().find("[L1 http_portal] station HTTP server started") != std::string::npos);
    REQUIRE(TestHttpdStartCount() == 1);
}

TEST_CASE("the station profile registers a 404 error handler and nothing for 405", "[T-3][FR-14]")
{
    ResetAll();
    REQUIRE(StartHttpStationServer(&kOps));
    REQUIRE(TestHttpdErrHandler(HTTPD_404_NOT_FOUND) != nullptr);
    REQUIRE(TestHttpdErrHandler(HTTPD_405_METHOD_NOT_ALLOWED) == nullptr);
    REQUIRE(TestHttpdErrHandler(HTTPD_400_BAD_REQUEST) == nullptr);
}

TEST_CASE("the station profile uses exact URI matching and the portal's socket, LRU and stack settings", "[T-3][FR-11]")
{
    ResetAll();
    REQUIRE(StartHttpStationServer(&kOps));
    const httpd_config_t *config = TestHttpdConfig();
    REQUIRE(config->uri_match_fn == nullptr);
    REQUIRE(config->server_port == 80);
    REQUIRE(config->max_open_sockets == 4);
    REQUIRE(config->lru_purge_enable);
    REQUIRE(config->stack_size == 5120);
    REQUIRE(config->max_uri_handlers >= 3);
}

TEST_CASE("the station profile registers no provisioning or captive-portal endpoint", "[T-3][FR-11][NFR-14]")
{
    ResetAll();
    REQUIRE(StartHttpStationServer(&kOps));
    for (const char *uri : {"/scan", "/submit", "/status", "/*"}) {
        INFO(uri);
        REQUIRE_FALSE(TestHttpdHasUri(uri));
    }
    for (const char *uri : kProbeUris) {
        INFO(uri);
        REQUIRE_FALSE(TestHttpdHasUri(uri));
    }
}

TEST_CASE("a URI registration failure stops the server and returns false", "[T-3][FR-11]")
{
    for (int failing = 0; failing < 3; ++failing) {
        INFO("failing registration " << failing);
        ResetAll();
        TestHttpdFailRegistrationAt(failing);
        REQUIRE_FALSE(StartHttpStationServer(&kOps));
        REQUIRE(TestHttpdStopCount() == 1);
        REQUIRE_FALSE(TestHttpdIsRunning());
        REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
        REQUIRE(Log().find("station HTTP server started") == std::string::npos);
    }
}

TEST_CASE("a 404 error handler registration failure stops the server and returns false", "[T-3][FR-11][FR-14]")
{
    ResetAll();
    TestHttpdFailErrHandler(true);
    REQUIRE_FALSE(StartHttpStationServer(&kOps));
    REQUIRE(TestHttpdStopCount() == 1);
    REQUIRE_FALSE(TestHttpdIsRunning());
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 1);
    REQUIRE(Log().find("station HTTP server started") == std::string::npos);
}

TEST_CASE("an httpd_start() failure returns false; NULL ops start nothing", "[T-3][FR-11]")
{
    ResetAll();
    TestHttpdFailStart(true);
    REQUIRE_FALSE(StartHttpStationServer(&kOps));
    REQUIRE(TestHttpdStopCount() == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);

    ResetAll();
    REQUIRE_FALSE(StartHttpStationServer(nullptr));
    REQUIRE(TestHttpdStartCount() == 0);
}

TEST_CASE("starting the station server while the portal runs stops the portal first", "[T-3][FR-7][FR-16]")
{
    ResetAll();
    REQUIRE(StartHttpPortal(&kOps));
    TestLogReset();
    REQUIRE(StartHttpStationServer(&kOps));

    REQUIRE(std::string(TestHttpdLifecycle()) == "start,stop,start");
    REQUIRE(Log().find("[L1 http_portal] portal HTTP server stopped") != std::string::npos);
    REQUIRE(Log().find("portal HTTP server stopped") < Log().find("station HTTP server started"));
    REQUIRE(TestHttpdHandlerCount() == 3);                         // only the station set is registered now
    REQUIRE_FALSE(TestHttpdHasUri("/scan"));
    REQUIRE_FALSE(TestHttpdHasUri("/*"));
}

TEST_CASE("starting the portal while the station server runs stops the station server first", "[T-3][FR-7][FR-16]")
{
    StartStation();
    REQUIRE(StartHttpPortal(&kOps));

    REQUIRE(std::string(TestHttpdLifecycle()) == "start,stop,start");
    REQUIRE(Log().find("[L1 http_portal] station HTTP server stopped") != std::string::npos);
    REQUIRE(TestHttpdErrHandler(HTTPD_404_NOT_FOUND) == nullptr);  // no station-only 404 in provisioning mode
    REQUIRE(TestHttpdConfig()->uri_match_fn == httpd_uri_match_wildcard);

    TestLogReset();
    TestHttpdRequest(HTTP_GET, "/foo", nullptr);                   // provisioning behavior: redirect, not 404
    REQUIRE(Status() == "302 Found");
    REQUIRE(Header("Location") == HTTP_PORTAL_URL);
}

TEST_CASE("restarting the station profile never runs two instances", "[T-3][FR-7]")
{
    StartStation();
    REQUIRE(StartHttpStationServer(&kOps));
    REQUIRE(std::string(TestHttpdLifecycle()) == "start,stop,start");
    REQUIRE(TestHttpdStartCount() - TestHttpdStopCount() == 1);
}

TEST_CASE("StopHttpPortal() logs the profile that was running and is safe when stopped", "[T-3][FR-16]")
{
    StartStation();
    StopHttpPortal();
    REQUIRE(Log() == "[L1 http_portal] station HTTP server stopped\n");
    REQUIRE(TestHttpdStopCount() == 1);

    TestLogReset();
    StopHttpPortal();                                              // nothing runs: no call, no log
    StopHttpPortal();
    REQUIRE(TestHttpdStopCount() == 1);
    REQUIRE(Log().empty());

    StartProvisioning();
    StopHttpPortal();
    REQUIRE(Log() == "[L1 http_portal] portal HTTP server stopped\n");
}

TEST_CASE("the provisioning-profile registration set is unchanged (regression)", "[T-3][FR-7][FR-30]")
{
    StartProvisioning();
    REQUIRE(TestHttpdHandlerCount() == 17);                        // 6 exact + 10 probes + catch-all
    REQUIRE(HasUri("/", HTTP_GET));
    REQUIRE(HasUri("/scan", HTTP_GET));
    REQUIRE(HasUri("/submit", HTTP_POST));
    REQUIRE(HasUri("/status", HTTP_GET));
    REQUIRE(HasUri("/tuner", HTTP_GET));
    REQUIRE(HasUri("/tuner", HTTP_POST));
    for (const char *uri : kProbeUris) {
        INFO(uri);
        REQUIRE(HasUri(uri, HTTP_ANY));
    }
    REQUIRE(std::string(TestHttpdUriAt(16)) == "/*");
    REQUIRE(TestHttpdConfig()->uri_match_fn == httpd_uri_match_wildcard);
    REQUIRE(TestHttpdErrHandler(HTTPD_404_NOT_FOUND) == nullptr);
}

// ==== T-4: station GET handlers, 404, 405 ============================================================================

TEST_CASE("GET / and GET /tuner serve the station page with the required headers", "[T-4][FR-12][FR-15]")
{
    StartStation();
    for (const char *uri : {"/", "/tuner"}) {
        INFO(uri);
        TestLogReset();
        REQUIRE(TestHttpdRequest(HTTP_GET, uri, nullptr) == ESP_OK);
        REQUIRE(Status() == "200 OK");
        REQUIRE(std::string(TestHttpdContentType()) == "text/html");
        REQUIRE(Header("Cache-Control") == "no-store");
        REQUIRE(Body() == g_tuner_page_station);
        REQUIRE(Body().find("Back") == std::string::npos);
        REQUIRE(Log() == "[L0 http_portal] serving tuner page\n");
    }
}

TEST_CASE("the station page does not depend on the Host header or a query string", "[T-4][FR-12]")
{
    StartStation();
    for (const char *host : {"rgb-tuner.local", "rgb-tuner-2.local", "192.168.1.42", "evil.example"}) {
        for (const char *uri : {"/", "/tuner", "/?x=1", "/tuner?b0h_ns=400"}) {
            INFO(host << " " << uri);
            TestHttpdSetRequestHeader("Host", host);
            TestHttpdRequest(HTTP_GET, uri, nullptr);
            REQUIRE(Status() == "200 OK");
            REQUIRE(Body() == g_tuner_page_station);
        }
    }
}

TEST_CASE("unknown paths get 404 Not found with no redirect and no URI echo", "[T-4][FR-14][NFR-14]")
{
    StartStation();
    std::vector<std::string> paths = {"/scan", "/submit", "/status", "/tuner/", "/favicon.ico", "/foo",
                                      "/index.html", "/marker-q7z-path"};
    for (const char *probe : kProbeUris) {
        paths.emplace_back(probe);
    }
    for (const std::string &path : paths) {
        for (int method : {HTTP_GET, HTTP_POST}) {
            INFO(path << " method " << method);
            TestLogReset();
            TestHttpdRequest(method, path.c_str(), method == HTTP_POST ? "a=1" : nullptr);
            REQUIRE(Status() == "404 Not Found");
            REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
            REQUIRE(Header("Cache-Control") == "no-store");
            REQUIRE(Body() == "Not found");
            REQUIRE(Header("Location") == "<none>");
            REQUIRE(Log() == "[L0 http_portal] station request not found\n");
        }
    }
    REQUIRE(std::string(TestHttpdAllOutput()).find("marker-q7z") == std::string::npos);
    REQUIRE(g_scan_calls == 0);
    REQUIRE(g_submit_calls == 0);
    REQUIRE(TestNvsOpCount() == 0);
}

TEST_CASE("PUT /tuner and POST / fall to the server's default 405, not to the 404 handler", "[T-4][FR-14]")
{
    StartStation();
    TestHttpdRequest(kHttpPut, "/tuner", "x");
    REQUIRE(Status() == "405 Method Not Allowed");
    TestHttpdRequest(HTTP_POST, "/", "x");
    REQUIRE(Status() == "405 Method Not Allowed");
    REQUIRE(Log().find("station request not found") == std::string::npos);
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
}

// ==== T-4 / FR-13: station POST /tuner with an allowed Origin runs the SPEC-003/004 path unchanged ==================

namespace {

struct TunerVector {
    const char *id;
    const char *body;
    const char *status;
    const char *response;
    const char *log;   // Info line text, or the rejection reason token
};

/** Vector S: a 97-byte body (TUNER_BODY_MAX + 1) made of vector A plus padding. */
const std::string kBodyS = std::string(kVectorA) + "&pad=" + std::string(TUNER_BODY_MAX + 1 - 56 - 5, 'x');

const TunerVector kVectors[] = {
    {"A", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "200 OK", "Sent", kVectorALog},
    {"B", "b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", "200 OK", "Sent",
     "tuner received: bit0 high_ns=100 period_ns=800; bit1 high_ns=100 period_ns=800; reset_us=50"},
    {"C", "b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", "200 OK", "Sent",
     "tuner received: bit0 high_ns=1075 period_ns=1200; bit1 high_ns=1100 period_ns=1200; reset_us=280"},
    {"D", "b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800", "200 OK", "Sent",
     "tuner received: bit0 high_ns=1200 period_ns=2000; bit1 high_ns=1000 period_ns=2000; reset_us=800"},
    {"E", "b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", "200 OK", "Sent", kVectorALog},
    {"F", "b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"G", "b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"H", "b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"I", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"J", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"K", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
    {"L", "b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", "400 Bad Request", "Invalid combination",
     "reason=bad_combination"},
    {"M", "b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Invalid combination",
     "reason=bad_combination"},
    {"N", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250", "400 Bad Request", "Invalid request", "reason=malformed"},
    {"O", "b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Invalid request",
     "reason=malformed"},
    {"P", "b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Invalid request",
     "reason=malformed"},
    {"Q", "b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Invalid request",
     "reason=malformed"},
    {"R", "b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "400 Bad Request", "Invalid request",
     "reason=malformed"},
    {"S", kBodyS.c_str(), "400 Bad Request", "Invalid request", "reason=malformed"},
    {"S0", "", "400 Bad Request", "Invalid request", "reason=malformed"},
    {"T", "b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", "200 OK", "Sent", kVectorALog},
    {"U", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280&x=1", "200 OK", "Sent", kVectorALog},
    {"V", "b0h_ns=90&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", "400 Bad Request", "Value out of range",
     "reason=out_of_range"},
};

struct Outcome {
    std::string status, body, content_type, cache, log;
    unsigned applies;
};

Outcome Capture()
{
    return {Status(), Body(), TestHttpdContentType(), Header("Cache-Control"), Log(), FakeApplyLedTiming_fake.call_count};
}

}  // namespace

TEST_CASE("SPEC-003 vectors A to V give the SPEC-003 responses and logs in the station profile", "[T-4][FR-13]")
{
    for (const TunerVector &vector : kVectors) {
        for (const char *origin : {static_cast<const char *>(nullptr), "http://rgb-tuner.local", "http://192.168.1.42"}) {
            INFO("vector " << vector.id << " origin " << (origin == nullptr ? "(absent)" : origin));
            StartStation();
            REQUIRE(std::strlen(vector.id) > 0);
            if (std::string(vector.id) == "S") {
                REQUIRE(std::strlen(vector.body) == TUNER_BODY_MAX + 1);   // the 97-byte body of vector S
            }
            PostTuner(origin, vector.body);
            const bool accepted = std::string(vector.status) == "200 OK";
            REQUIRE(Status() == vector.status);
            REQUIRE(Body() == vector.response);
            REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
            REQUIRE(Header("Cache-Control") == "no-store");
            REQUIRE(Log().find(vector.log) != std::string::npos);
            REQUIRE(TestLogCount(accepted ? LOG_LEVEL_INFO : LOG_LEVEL_WARNING) == 1);
            REQUIRE(TestLogCount(accepted ? LOG_LEVEL_WARNING : LOG_LEVEL_INFO) == 0);
            REQUIRE(FakeApplyLedTiming_fake.call_count == (accepted ? 1u : 0u));
            REQUIRE(Log().find("foreign_origin") == std::string::npos);
        }
    }
}

TEST_CASE("an allowed station POST /tuner behaves byte for byte like the provisioning one", "[T-4][FR-13]")
{
    for (const TunerVector &vector : kVectors) {
        INFO("vector " << vector.id);
        StartProvisioning();
        PostTuner(nullptr, vector.body);
        const Outcome provisioning = Capture();

        StartStation();
        PostTuner("http://rgb-tuner-2.local", vector.body);
        const Outcome station = Capture();

        REQUIRE(station.status == provisioning.status);
        REQUIRE(station.body == provisioning.body);
        REQUIRE(station.content_type == provisioning.content_type);
        REQUIRE(station.cache == provisioning.cache);
        REQUIRE(station.log == provisioning.log);
        REQUIRE(station.applies == provisioning.applies);
    }
}

namespace {
ws2812_timing_t g_applied_timing;
void CaptureAppliedTiming(const ws2812_timing_t *timing) { g_applied_timing = *timing; }
}  // namespace

TEST_CASE("an allowed request hands the parsed timing set to apply_led_timing", "[T-4][FR-13]")
{
    StartStation();
    g_applied_timing = {};
    FakeApplyLedTiming_fake.custom_fake = CaptureAppliedTiming;
    PostTuner("http://rgb-tuner.local", "b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800");
    REQUIRE(FakeApplyLedTiming_fake.call_count == 1);
    const ws2812_timing_t expected = {1200, 2000, 1000, 2000, 800};
    REQUIRE(std::memcmp(&g_applied_timing, &expected, sizeof(expected)) == 0);
}

// ==== T-20: the Origin check in the handler (FR-27, FR-28, FR-29) ===================================================

namespace {

struct OriginVector {
    int number;
    const char *origin;   // nullptr: header absent
    const char *hostname;
    uint32_t station_ipv4;
    bool allowed;
};

const std::string kLongOrigin = "http://rgb-tuner.local" + std::string(97 - 22, 'a');

std::vector<OriginVector> OriginVectors()
{
    return {
        {1, nullptr, "rgb-tuner-2", kStationIp, true},
        {2, "http://rgb-tuner.local", "rgb-tuner-2", kStationIp, true},
        {3, "http://rgb-tuner-2.local", "rgb-tuner-2", kStationIp, true},
        {4, "http://192.168.1.42", "rgb-tuner-2", kStationIp, true},
        {5, "http://rgb-tuner.local:80", "rgb-tuner-2", kStationIp, true},
        {6, "http://192.168.1.42:80", "rgb-tuner-2", kStationIp, true},
        {7, "HTTP://RGB-TUNER.LOCAL", "rgb-tuner-2", kStationIp, true},
        {8, "null", "rgb-tuner-2", kStationIp, false},
        {9, "", "rgb-tuner-2", kStationIp, false},
        {10, "http://evil.example", "rgb-tuner-2", kStationIp, false},
        {11, "https://rgb-tuner.local", "rgb-tuner-2", kStationIp, false},
        {12, "http://rgb-tuner.local:8080", "rgb-tuner-2", kStationIp, false},
        {13, "http://rgb-tuner.local:", "rgb-tuner-2", kStationIp, false},
        {14, "http://rgb-tuner.local/", "rgb-tuner-2", kStationIp, false},
        {15, "http://rgb-tuner.local.evil.example", "rgb-tuner-2", kStationIp, false},
        {16, "http://evil-rgb-tuner.local", "rgb-tuner-2", kStationIp, false},
        {17, "http://rgb-tuner-3.local", "rgb-tuner-2", kStationIp, false},
        {18, "http://192.168.1.43", "rgb-tuner-2", kStationIp, false},
        {19, "http://192.168.001.042", "rgb-tuner-2", kStationIp, false},
        {20, kLongOrigin.c_str(), "rgb-tuner-2", kStationIp, false},
        {21, "http://rgb-tuner-2.local", "rgb-tuner", kStationIp, false},
        {22, "http://0.0.0.0", "rgb-tuner-2", 0, false},
    };
}

}  // namespace

TEST_CASE("all 22 section 7.5 Origin vectors through the station POST /tuner handler with vector A", "[T-20][FR-27][FR-28]")
{
    REQUIRE(kLongOrigin.size() == 97);
    for (const OriginVector &vector : OriginVectors()) {
        INFO("Origin vector " << vector.number);
        StartStation(vector.hostname, vector.station_ipv4);
        PostTuner(vector.origin);
        REQUIRE(TestHttpdHeaderReadCount() == 1);                   // the Origin header is read once
        if (vector.allowed) {
            RequireVectorAAccepted();
        } else {
            RequireForeignRejection(vector.origin == nullptr ? "" : (std::strlen(vector.origin) > 8 ? vector.origin : ""));
        }
    }
}

TEST_CASE("a foreign Origin is not reflected in the response or the log (unique marker)", "[T-20][FR-28][NFR-14]")
{
    StartStation();
    PostTuner("http://marker-x9.example");
    RequireForeignRejection("marker-x9");
}

TEST_CASE("a foreign Origin is rejected before the body is looked at, whatever the body", "[T-20][FR-28]")
{
    for (const char *body : {kVectorA, "", "b0h_ns=90", "garbage-body-that-is-far-too-long-for-the-tuner-form-limit-"
                                                        "of-ninety-six-bytes-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}) {
        INFO(body);
        StartStation();
        TestHttpdRecvTimeouts(5);                                  // would matter only if the body were read
        PostTuner("http://evil.example", body);
        RequireForeignRejection("evil.example");
    }
}

TEST_CASE("the handler takes and gives the identity mutex once per request", "[T-20][FR-29][NFR-9]")
{
    StartStation();
    const int takes = HarnessGetIdentityMutexTakes();
    const int gives = HarnessGetIdentityMutexGives();
    REQUIRE(takes == gives);

    PostTuner("http://rgb-tuner-2.local");
    REQUIRE(HarnessGetIdentityMutexTakes() == takes + 1);
    REQUIRE(HarnessGetIdentityMutexGives() == gives + 1);

    PostTuner("http://evil.example");
    REQUIRE(HarnessGetIdentityMutexTakes() == takes + 2);
    REQUIRE(HarnessGetIdentityMutexGives() == gives + 2);

    PostTuner(nullptr);
    REQUIRE(HarnessGetIdentityMutexTakes() == takes + 3);
    REQUIRE(HarnessGetIdentityMutexGives() == gives + 3);
    REQUIRE(MockGetMutexBalance() == 0);

    TestHttpdRequest(HTTP_GET, "/", nullptr);                      // GET never touches the identity
    REQUIRE(HarnessGetIdentityMutexTakes() == takes + 3);
}

TEST_CASE("SetHttpStationIdentity() writes the record under the mutex and creates it once", "[T-20][FR-29]")
{
    ResetAll();
    REQUIRE_FALSE(HarnessHasIdentityMutex());
    SetHttpStationIdentity("rgb-tuner", kStationIp);
    REQUIRE(HarnessHasIdentityMutex());
    REQUIRE(HarnessGetIdentityMutexTakes() == 1);
    REQUIRE(HarnessGetIdentityMutexGives() == 1);
    REQUIRE(std::string(HarnessGetIdentityName()) == "rgb-tuner");
    REQUIRE(HarnessGetIdentityAddress() == kStationIp);

    SetHttpStationIdentity("rgb-tuner-2", Ipv4(192, 168, 1, 50));
    REQUIRE(HarnessGetIdentityMutexTakes() == 2);                  // same mutex, not re-created (counts kept)
    REQUIRE(HarnessGetIdentityMutexGives() == 2);
    REQUIRE(std::string(HarnessGetIdentityName()) == "rgb-tuner-2");

    SetHttpStationIdentity(nullptr, 0);                            // NULL stores an empty name
    REQUIRE(std::string(HarnessGetIdentityName()).empty());
    REQUIRE(HarnessGetIdentityAddress() == 0);

    const std::string too_long(80, 'z');                           // truncated to 63 characters
    SetHttpStationIdentity(too_long.c_str(), kStationIp);
    REQUIRE(std::string(HarnessGetIdentityName()) == std::string(MDNS_SERVICE_HOSTNAME_MAX - 1, 'z'));
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("an address change moves the accepted IP origin", "[T-20][FR-29]")
{
    StartStation("rgb-tuner-2", kStationIp);
    PostTuner("http://192.168.1.42");
    REQUIRE(Status() == "200 OK");

    SetHttpStationIdentity("rgb-tuner-2", Ipv4(192, 168, 1, 50));
    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://192.168.1.42");
    RequireForeignRejection("192.168.1.42");

    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://192.168.1.50");
    RequireVectorAAccepted();
}

TEST_CASE("a rename becomes accepted once the identity carries the new name", "[T-20][FR-29]")
{
    StartStation("rgb-tuner", kStationIp);
    PostTuner("http://rgb-tuner-2.local");
    REQUIRE(Status() == "403 Forbidden");                          // the accepted window before the 3 s check

    SetHttpStationIdentity("rgb-tuner-2", kStationIp);             // FR-29 c after the hostname check
    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://rgb-tuner-2.local");
    RequireVectorAAccepted();
    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://rgb-tuner.local");                           // the default name stays allowed
    RequireVectorAAccepted();
}

TEST_CASE("before any identity is set, only an absent Origin or the default name is allowed", "[T-20][FR-27][FR-29]")
{
    ResetAll();
    REQUIRE(StartHttpStationServer(&kOps));
    REQUIRE_FALSE(HarnessHasIdentityMutex());

    TestLogReset();
    PostTuner(nullptr);
    RequireVectorAAccepted();

    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://rgb-tuner.local");
    RequireVectorAAccepted();

    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    PostTuner("http://192.168.1.42");
    RequireForeignRejection("192.168.1.42");
}

TEST_CASE("the Origin header name is matched case-insensitively; Referer is ignored", "[T-20][FR-27]")
{
    StartStation();
    TestHttpdSetRequestHeader("Referer", "http://evil.example/page");
    TestHttpdSetRequestHeader("origin", "http://rgb-tuner.local");
    Request(HTTP_POST, "/tuner", kVectorA);
    RequireVectorAAccepted();

    TestLogReset();
    RESET_FAKE(FakeApplyLedTiming);
    TestHttpdClearRequestHeaders();
    TestHttpdSetRequestHeader("Referer", "http://rgb-tuner.local/");
    TestHttpdSetRequestHeader("ORIGIN", "http://evil.example");
    Request(HTTP_POST, "/tuner", kVectorA);
    RequireForeignRejection("evil.example");
}

TEST_CASE("repeated foreign requests each log exactly one Warning", "[T-20][FR-28]")
{
    StartStation();
    for (int request = 0; request < 5; ++request) {
        PostTuner("http://evil.example");
    }
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 5);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
    REQUIRE(TestHttpdRecvCount() == 0);
}

// ==== FR-30: the provisioning profile performs no Origin check ======================================================

TEST_CASE("provisioning POST /tuner with a foreign Origin still returns 200 Sent (vector A)", "[T-20][FR-30]")
{
    StartProvisioning();
    PostTuner("http://evil.example");
    RequireVectorAAccepted();
    REQUIRE(TestHttpdHeaderReadCount() == 0);                      // no Origin check at all

    for (const char *origin : {"null", "", "https://rgb-tuner.local", "http://marker-x9.example"}) {
        INFO(origin);
        TestLogReset();
        RESET_FAKE(FakeApplyLedTiming);
        PostTuner(origin);
        RequireVectorAAccepted();
    }
    REQUIRE(TestHttpdHeaderReadCount() == 0);
}

TEST_CASE("the identity does not influence the provisioning profile", "[T-20][FR-30]")
{
    ResetAll();
    SetHttpStationIdentity("rgb-tuner", 0);
    REQUIRE(StartHttpPortal(&kOps));
    TestLogReset();
    const int takes = HarnessGetIdentityMutexTakes();
    PostTuner("http://evil.example");
    RequireVectorAAccepted();
    REQUIRE(HarnessGetIdentityMutexTakes() == takes);
}
