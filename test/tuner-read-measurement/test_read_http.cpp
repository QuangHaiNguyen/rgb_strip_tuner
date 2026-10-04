/**
 * @file test_read_http.cpp
 * @brief SPEC-006 T-3 (FR-7, FR-8, NFR-10) and the http_portal part of T-4 (FR-9, FR-10): `POST /tuner/read` in the
 *        provisioning and station profiles through the httpd simulator.
 *
 * http_portal.c is compiled through test/captive-portal/mocks/http_portal_harness.c so its statics (server, profile,
 * submission counter, result record) are reset per case. http_portal_ops_t::apply_led_timing and request_pulse_read
 * are FFF fakes; LogWrite() is the FFF fake of test/ws2812-tuner-page.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fff.h"   // FFF globals live in test/ws2812-tuner-page/mocks/log_fakes.c

extern "C" {
#include "freertos_mock.h"
#include "http_portal.h"
#include "httpd_mock.h"
#include "log_fakes.h"
#include "logging.h"
#include "nvs_fake.h"
#include "ws2812_timing.h"
void HarnessResetHttpPortal(void);
uint32_t HarnessGetSubmitSeq(void);
ws2812_measurement_t HarnessGetTunerResult(void);
}

namespace {

uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t octets[4] = {a, b, c, d};
    uint32_t address = 0;
    std::memcpy(&address, octets, sizeof(address));
    return address;
}

const uint32_t kStationIp = Ipv4(192, 168, 1, 42);
const char *const kVectorA = "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280";

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

FAKE_VOID_FUNC(FakeApplyLedTiming, const ws2812_timing_t *, uint32_t);
FAKE_VOID_FUNC(FakeRequestPulseRead, uint32_t);

/** What had happened when request_pulse_read ran: FR-8 orders steps (3), (4) before (5) before (6). */
struct ReadCall {
    uint32_t submit_seq;
    int debug_logs;
    int info_logs;
    std::string log_text;
    std::string body_at_call;   // the 200 Reading response must not have been sent yet
};
std::vector<ReadCall> g_read_calls;

void RecordReadCall(uint32_t submit_seq)
{
    g_read_calls.push_back({submit_seq, TestLogCount(LOG_LEVEL_DEBUG), TestLogCount(LOG_LEVEL_INFO), TestLogText(),
                            TestHttpdBody()});
}

const http_portal_ops_t kOps = {FakeScan, FakeSubmit, FakeApplyLedTiming, FakeRequestPulseRead};

void ResetAll()
{
    MockFreeRtosReset();
    TestHttpdReset();
    TestHttpdClearRequestHeaders();
    TestNvsReset();
    HarnessResetHttpPortal();
    TestLogReset();
    g_scan_calls = 0;
    g_submit_calls = 0;
    RESET_FAKE(FakeApplyLedTiming);
    RESET_FAKE(FakeRequestPulseRead);
    FakeRequestPulseRead_fake.custom_fake = RecordReadCall;
    g_read_calls.clear();
}

void StartProvisioning()
{
    ResetAll();
    REQUIRE(StartHttpPortal(&kOps));
    TestLogReset();
}

void StartStation()
{
    ResetAll();
    SetHttpStationIdentity("rgb-tuner-2", kStationIp);
    REQUIRE(StartHttpStationServer(&kOps));
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

esp_err_t PostRead(const char *body = nullptr) { return TestHttpdRequest(HTTP_POST, "/tuner/read", body); }
esp_err_t PostTuner(const char *body = kVectorA) { return TestHttpdRequest(HTTP_POST, "/tuner", body); }

int IndexOf(const char *uri, int method)
{
    for (int index = 0; index < TestHttpdHandlerCount(); ++index) {
        if (std::string(TestHttpdUriAt(index)) == uri && TestHttpdMethodAt(index) == method) {
            return index;
        }
    }
    return -1;
}

void RequireReadAccepted(uint32_t seq)
{
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Reading");
    REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(Header("Cache-Control") == "no-store");
    REQUIRE(Header("Tuner-Seq") == std::to_string(seq));
    REQUIRE(std::string(TestHttpdHeaderNames()) == "Cache-Control,Tuner-Seq");   // no CORS or other header
}

std::string ReadSource(const char *path)
{
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

/** Body of the static C function @p signature in @p source, up to the closing brace at column 0. */
std::string FunctionBody(const std::string &source, const std::string &signature)
{
    const size_t begin = source.find(signature);
    REQUIRE(begin != std::string::npos);
    const size_t end = source.find("\n}\n", begin);
    REQUIRE(end != std::string::npos);
    return source.substr(begin, end - begin);
}

}  // namespace

// ==== FR-7: registration ============================================================================================

TEST_CASE("provisioning: 19 handlers against a limit of 20, POST /tuner/read before the /* catch-all", "[T-3][FR-7]")
{
    StartProvisioning();
    REQUIRE(TestHttpdHandlerCount() == 19);
    REQUIRE(TestHttpdConfig()->max_uri_handlers == 20);
    const int read_at = IndexOf("/tuner/read", HTTP_POST);
    REQUIRE(read_at >= 0);
    REQUIRE(std::string(TestHttpdUriAt(18)) == "/*");
    REQUIRE(read_at < 18);
    // Exactly one registration of the URI, POST only (other methods fall to the catch-all).
    int count = 0;
    for (int index = 0; index < TestHttpdHandlerCount(); ++index) {
        count += std::string(TestHttpdUriAt(index)) == "/tuner/read";
    }
    REQUIRE(count == 1);
}

TEST_CASE("station: exactly 5 handlers against a limit of 5, including POST /tuner/read", "[T-3][FR-7]")
{
    StartStation();
    REQUIRE(TestHttpdHandlerCount() == 5);
    REQUIRE(TestHttpdConfig()->max_uri_handlers == 5);
    REQUIRE(IndexOf("/", HTTP_GET) >= 0);
    REQUIRE(IndexOf("/tuner", HTTP_GET) >= 0);
    REQUIRE(IndexOf("/tuner", HTTP_POST) >= 0);
    REQUIRE(IndexOf("/tuner/read", HTTP_POST) >= 0);
    REQUIRE(IndexOf("/tuner/result", HTTP_GET) >= 0);
    REQUIRE_FALSE(TestHttpdHasUri("/*"));
}

TEST_CASE("every registration result is checked: a failing /tuner/read registration stops the server", "[T-3][FR-7]")
{
    SECTION("provisioning")
    {
        ResetAll();
        REQUIRE(StartHttpPortal(&kOps));
        const int read_at = IndexOf("/tuner/read", HTTP_POST);
        REQUIRE(read_at >= 0);
        ResetAll();
        TestHttpdFailRegistrationAt(read_at);
        REQUIRE_FALSE(StartHttpPortal(&kOps));
        REQUIRE_FALSE(TestHttpdIsRunning());
        REQUIRE(Log().find("failed to register handler for /tuner/read") != std::string::npos);
    }
    SECTION("station")
    {
        ResetAll();
        SetHttpStationIdentity("rgb-tuner-2", kStationIp);
        REQUIRE(StartHttpStationServer(&kOps));
        const int read_at = IndexOf("/tuner/read", HTTP_POST);
        REQUIRE(read_at >= 0);
        ResetAll();
        SetHttpStationIdentity("rgb-tuner-2", kStationIp);
        TestHttpdFailRegistrationAt(read_at);
        REQUIRE_FALSE(StartHttpStationServer(&kOps));
        REQUIRE_FALSE(TestHttpdIsRunning());
        REQUIRE(Log().find("failed to register handler for /tuner/read") != std::string::npos);
    }
    TestHttpdFailRegistrationAt(-1);
}

TEST_CASE("provisioning: GET /tuner/read and /tuner/read/ fall to the catch-all 302", "[T-3][FR-7]")
{
    StartProvisioning();
    for (const auto &request : {std::make_pair(HTTP_GET, "/tuner/read"), std::make_pair(HTTP_GET, "/tuner/read/"),
                                std::make_pair(HTTP_POST, "/tuner/read/")}) {
        INFO(request.second);
        TestHttpdRequest(request.first, request.second, nullptr);
        REQUIRE(Status() == "302 Found");
    }
    REQUIRE(FakeRequestPulseRead_fake.call_count == 0);
    REQUIRE(HarnessGetSubmitSeq() == 0);
}

TEST_CASE("station: GET /tuner/read gets 405 and /tuner/read/ gets the FR-14 404", "[T-3][FR-7]")
{
    StartStation();
    TestHttpdRequest(HTTP_GET, "/tuner/read", nullptr);
    REQUIRE(Status() == "405 Method Not Allowed");
    TestHttpdRequest(HTTP_GET, "/tuner/read/", nullptr);
    REQUIRE(Status() == "404 Not Found");
    TestHttpdRequest(HTTP_POST, "/tuner/read/", nullptr);
    REQUIRE(Status() == "404 Not Found");
    REQUIRE(FakeRequestPulseRead_fake.call_count == 0);
    REQUIRE(HarnessGetSubmitSeq() == 0);
}

// ==== FR-8: POST /tuner/read ========================================================================================

TEST_CASE("POST /tuner/read: 200, text/plain, no-store, Tuner-Seq, body Reading (both profiles)", "[T-3][FR-8]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station, no Origin header")
    {
        StartStation();
    }
    SECTION("station, same-origin hostname")
    {
        StartStation();
        TestHttpdSetRequestHeader("Origin", "http://rgb-tuner-2.local");
    }
    SECTION("station, same-origin address")
    {
        StartStation();
        TestHttpdSetRequestHeader("Origin", "http://192.168.1.42");
    }
    REQUIRE(PostRead() == ESP_OK);
    RequireReadAccepted(1);
    REQUIRE(FakeRequestPulseRead_fake.call_count == 1);
    REQUIRE(FakeRequestPulseRead_fake.arg0_val == 1);
    REQUIRE(HarnessGetSubmitSeq() == 1);
}

TEST_CASE("POST /tuner/read logs Debug 'tuner submission seq=<n>' then Info 'tuner read requested', nothing else", "[T-3][FR-8][NFR-9]")
{
    StartProvisioning();
    PostRead();
    REQUIRE(Log() == "[L0 http_portal] tuner submission seq=1\n[L1 http_portal] tuner read requested\n");
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 0);
    REQUIRE(TestLogCount(LOG_LEVEL_ERROR) == 0);
}

TEST_CASE("FR-8 order: the number and both log lines precede request_pulse_read, which precedes the response", "[T-3][FR-8]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station") { StartStation(); }
    PostRead();
    REQUIRE(g_read_calls.size() == 1);
    REQUIRE(g_read_calls[0].submit_seq == 1);
    REQUIRE(g_read_calls[0].debug_logs == 1);
    REQUIRE(g_read_calls[0].info_logs == 1);
    REQUIRE(g_read_calls[0].log_text.find("tuner submission seq=1") < g_read_calls[0].log_text.find("tuner read requested"));
    REQUIRE(g_read_calls[0].body_at_call.empty());   // the 200 Reading had not been sent yet
}

TEST_CASE("the request body is never read; any body or Content-Length is ignored", "[T-3][FR-8]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station") { StartStation(); }
    const int recv_before = TestHttpdRecvCount();
    PostRead("b0h_ns=100&junk=" "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    REQUIRE(TestHttpdRecvCount() == recv_before);
    RequireReadAccepted(1);
    PostRead(nullptr);
    REQUIRE(TestHttpdRecvCount() == recv_before);
    RequireReadAccepted(2);
    REQUIRE(std::string(TestHttpdAllOutput()).find("junk") == std::string::npos);   // NFR-10: nothing reflected
    REQUIRE(Log().find("junk") == std::string::npos);
}

TEST_CASE("Tuner-Seq is shared with POST /tuner: POST, READ, POST -> 1, 2, 3", "[T-3][FR-8]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station") { StartStation(); }
    PostTuner();
    REQUIRE(Body() == "Sent");
    REQUIRE(Header("Tuner-Seq") == "1");
    PostRead();
    RequireReadAccepted(2);
    PostTuner();
    REQUIRE(Body() == "Sent");
    REQUIRE(Header("Tuner-Seq") == "3");

    REQUIRE(FakeApplyLedTiming_fake.call_count == 2);
    REQUIRE(FakeApplyLedTiming_fake.arg1_history[0] == 1);
    REQUIRE(FakeApplyLedTiming_fake.arg1_history[1] == 3);
    REQUIRE(FakeRequestPulseRead_fake.call_count == 1);
    REQUIRE(FakeRequestPulseRead_fake.arg0_val == 2);
    REQUIRE(HarnessGetSubmitSeq() == 3);
}

TEST_CASE("consecutive reads get increasing numbers 1, 2, 3", "[T-3][FR-8]")
{
    StartProvisioning();
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        PostRead();
        RequireReadAccepted(expected);
        REQUIRE(FakeRequestPulseRead_fake.arg0_history[expected - 1] == expected);
    }
}

TEST_CASE("FR-9: a read never calls apply_led_timing, scan or submit, and touches no NVS", "[T-4][FR-9]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station") { StartStation(); }
    for (int index = 0; index < 3; ++index) {
        PostRead();
    }
    REQUIRE(FakeRequestPulseRead_fake.call_count == 3);
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
    REQUIRE(g_scan_calls == 0);
    REQUIRE(g_submit_calls == 0);
    REQUIRE(Log().find("tuner received") == std::string::npos);   // no SPEC-003 timing line
}

TEST_CASE("station foreign Origin: 403, Warning, counter unchanged, no request_pulse_read", "[T-3][FR-8][NFR-10]")
{
    StartStation();
    for (const char *origin : {"http://evil.example", "http://rgb-tuner-3.local", "http://192.168.1.50", "null",
                               "https://rgb-tuner-2.local"}) {
        INFO(origin);
        TestLogReset();
        const int recv_before = TestHttpdRecvCount();
        TestHttpdSetRequestHeader("Origin", origin);
        PostRead("b0h_ns=1");
        REQUIRE(Status() == "403 Forbidden");
        REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
        REQUIRE(Header("Cache-Control") == "no-store");
        REQUIRE(Body() == "Forbidden origin");
        REQUIRE(Header("Tuner-Seq") == "<none>");
        REQUIRE(Log() == "[L2 http_portal] tuner read rejected: reason=foreign_origin\n");   // exactly one line
        REQUIRE(TestHttpdRecvCount() == recv_before);
        REQUIRE(Log().find(origin) == std::string::npos);
    }
    REQUIRE(HarnessGetSubmitSeq() == 0);
    REQUIRE(FakeRequestPulseRead_fake.call_count == 0);
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);

    // A rejected request consumed no number: the next accepted read gets 1.
    TestHttpdSetRequestHeader("Origin", "http://rgb-tuner-2.local");
    PostRead();
    RequireReadAccepted(1);
}

TEST_CASE("station: the POST /tuner rejection text is unchanged and distinct from the read rejection", "[T-3][FR-8][SPEC-005]")
{
    StartStation();
    TestHttpdSetRequestHeader("Origin", "http://evil.example");
    PostTuner();
    REQUIRE(Log() == "[L2 http_portal] tuner request rejected: reason=foreign_origin\n");
    REQUIRE(Body() == "Forbidden origin");
}

TEST_CASE("provisioning performs no Origin check on POST /tuner/read", "[T-3][FR-8]")
{
    StartProvisioning();
    TestHttpdSetRequestHeader("Origin", "http://evil.example");
    const int header_reads_before = TestHttpdHeaderReadCount();
    PostRead();
    RequireReadAccepted(1);
    REQUIRE(TestHttpdHeaderReadCount() == header_reads_before);   // Origin never even looked up
    REQUIRE(Log().find("rejected") == std::string::npos);
}

TEST_CASE("end to end: a READ_DONE record for the read's number is served as state=read", "[T-3][FR-8][FR-19]")
{
    SECTION("provisioning") { StartProvisioning(); }
    SECTION("station") { StartStation(); }
    PostRead();
    RequireReadAccepted(1);
    TestHttpdRequest(HTTP_GET, "/tuner/result?seq=1", nullptr);
    REQUIRE(Body() == "state=pending");

    ws2812_measurement_t read = {};
    read.submit_seq = 1;
    read.state = WS2812_MEASUREMENT_READ_DONE;
    read.bit0_high_avg_ns = 400;
    read.bit0_period_avg_ns = 1250;
    read.bit1_high_avg_ns = 800;
    read.bit1_period_avg_ns = 1250;
    SetHttpTunerResult(&read);
    TestHttpdRequest(HTTP_GET, "/tuner/result?seq=1", nullptr);
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "state=read&b0h=400&b0p=1250&b1h=800&b1p=1250");

    read.bit0_high_avg_ns = 0;
    read.bit0_period_avg_ns = 0;
    SetHttpTunerResult(&read);
    TestHttpdRequest(HTTP_GET, "/tuner/result?seq=1", nullptr);
    REQUIRE(Body() == "state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250");
}

TEST_CASE("end to end: the 70-byte read body is served whole through the handler", "[T-3][FR-19]")
{
    StartProvisioning();
    PostRead();
    ws2812_measurement_t read = {};
    read.submit_seq = 1;
    read.state = WS2812_MEASUREMENT_READ_DONE;
    read.bit0_high_avg_ns = read.bit0_period_avg_ns = read.bit1_high_avg_ns = read.bit1_period_avg_ns = 4294967295u;
    SetHttpTunerResult(&read);
    TestHttpdRequest(HTTP_GET, "/tuner/result?seq=1", nullptr);
    REQUIRE(Body() == "state=read&b0h=4294967295&b0p=4294967295&b1h=4294967295&b1p=4294967295");
}

TEST_CASE("static: the read handlers reference no apply_led_timing, body read, NVS, GPIO or RMT call", "[T-4][FR-8][FR-9]")
{
    const std::string source = ReadSource(HTTP_PORTAL_SRC);
    for (const char *signature : {"static esp_err_t HandleTunerReadRequest(httpd_req_t *request)",
                                  "static esp_err_t HandleStationTunerReadRequest(httpd_req_t *request)"}) {
        INFO(signature);
        const std::string body = FunctionBody(source, signature);
        for (const char *token : {"apply_led_timing", "httpd_req_recv", "nvs_", "gpio_", "rmt_", "ApplyWs2812Timing",
                                  "ArmPulseCapture", "malloc"}) {
            INFO(token);
            REQUIRE(body.find(token) == std::string::npos);
        }
    }
}
