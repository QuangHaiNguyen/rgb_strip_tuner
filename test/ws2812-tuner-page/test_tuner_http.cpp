/**
 * @file test_tuner_http.cpp
 * @brief Host tests for the GET/POST /tuner handlers through the http_portal simulator
 *        (SPEC-003 T-4, T-12; FR-17..FR-20, NFR-11), plus vector S (FR-15 body-length cap).
 *
 * T-4 (FR-19, FR-20 no-side-effect guard): main/ws2812_timing/ws2812_timing.c and the tuner
 * handlers of main/http_portal/http_portal.c reference no NVS, GPIO, RMT/peripheral, or
 * provisioning-queue symbol at all (grep check below, and confirmed by inspection: nvs.h,
 * driver/gpio.h, driver/rmt*.h and provisioning.h are not included by either file). FFF fakes for
 * those APIs are therefore not linked into this binary: there is nothing in the compiled tuner
 * code path for such a fake to be called by. This is a stronger guarantee than a runtime
 * call-count of zero on a fake that the code could never reach. The one owner service the tuner
 * handlers could reach (http_portal_ops_t, i.e. the SPEC-002 scan/submit callbacks) and the
 * in-memory NVS fake reused from test/captive-portal are exercised below to confirm a zero call
 * count directly.
 *
 * SPEC-004 FR-4 supersedes FR-19's "no side effect" clause for a *valid* submission only: the
 * handler now also calls http_portal_ops_t::apply_led_timing once. The SPEC-004 T-4 cases at the
 * end of this file check that call through an FFF fake; rejected requests and GET still make none.
 *
 * 2026-10-03 revision (SPEC-003 T-15, T-16, T-17 handler part, T-18 host part): rule V4 through the handler for all
 * vectors A to AF, the submission counter and `Tuner-Seq` header, GET /tuner/result in the provisioning profile,
 * SetHttpTunerResult() and the result mutex. The station profile's copy of these checks is in
 * test/station-mdns-tuner/test_station_http.cpp.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "fff.h"   // FFF globals live in mocks/log_fakes.c

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
bool HarnessHasResultMutex(void);
int HarnessGetResultMutexTakes(void);
int HarnessGetResultMutexGives(void);
ws2812_measurement_t HarnessGetTunerResult(void);
}

namespace {

int g_scan_calls = 0;
int g_submit_calls = 0;

int FakeScan(wifi_scan_entry_t *entries, uint16_t max_entries)
{
    (void)entries;
    (void)max_entries;
    ++g_scan_calls;
    return 0;
}

bool FakeSubmit(const wifi_credentials_t *credentials)
{
    (void)credentials;
    ++g_submit_calls;
    return true;
}

// SPEC-004 FR-4: a valid POST /tuner hands the parsed timing set to apply_led_timing. http_portal.c calls it without
// a NULL check (like submit_credentials), so the harness must supply it. The handler memsets its local timing right
// after the call, so the custom fake copies the value and also snapshots what had happened by the time of the call.
FAKE_VOID_FUNC(FakeApplyLedTiming, const ws2812_timing_t *, uint32_t);

struct AppliedCall {
    ws2812_timing_t timing;
    uint32_t submit_seq;            // SPEC-003 FR-23 / SPEC-004 FR-4 (2026-10-03)
    int info_logs_at_call;          // the FR-17 Info line must already have been written
    std::string body_at_call;       // the 200 Sent response must not have been sent yet
};
std::vector<AppliedCall> g_applied;

void RecordAppliedTiming(const ws2812_timing_t *timing, uint32_t submit_seq)
{
    g_applied.push_back({*timing, submit_seq, TestLogCount(LOG_LEVEL_INFO), TestHttpdBody()});
}

const http_portal_ops_t kOps = {FakeScan, FakeSubmit, FakeApplyLedTiming, nullptr};   // request_pulse_read: SPEC-006, unused here

void StartPortal()
{
    MockFreeRtosReset();
    TestHttpdReset();
    TestNvsReset();
    HarnessResetHttpPortal();
    g_scan_calls = 0;
    g_submit_calls = 0;
    RESET_FAKE(FakeApplyLedTiming);
    FakeApplyLedTiming_fake.custom_fake = RecordAppliedTiming;
    g_applied.clear();
    REQUIRE(StartHttpPortal(&kOps));
    TestLogReset();   // discard StartHttpPortal()'s own "portal HTTP server started" Info line
}

std::string Status() { return TestHttpdStatus(); }
std::string Body() { return TestHttpdBody(); }

esp_err_t Get() { return TestHttpdRequest(HTTP_GET, "/tuner", nullptr); }
esp_err_t Post(const std::string &body) { return TestHttpdRequest(HTTP_POST, "/tuner", body.c_str()); }

/** @brief Read a whole source file into a string, for the T-4 grep-style static check. */
std::string ReadSource(const char *path)
{
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

// ---- Vectors exercised end to end, through the HTTP handlers ------------------------------------------------------

TEST_CASE("vector A end to end: 200 Sent with the FR-17 Info line", "[T-4][FR-17]")
{
    StartPortal();
    REQUIRE(Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280") == ESP_OK);

    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Sent");
    REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 1);
    REQUIRE(std::string(TestLogText()).find(
                "tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280") !=
            std::string::npos);
}

TEST_CASE("vector F end to end: 400 Value out of range", "[T-4][FR-18]")
{
    StartPortal();
    Post("b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Value out of range");
    REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
    REQUIRE(std::string(TestLogText()).find("reason=out_of_range") != std::string::npos);
}

TEST_CASE("vector L end to end: 400 Invalid combination", "[T-4][FR-18]")
{
    StartPortal();
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid combination");
    REQUIRE(std::string(TestLogText()).find("reason=bad_combination") != std::string::npos);
}

TEST_CASE("vector N end to end: 400 Invalid request for a missing key", "[T-4][FR-18]")
{
    StartPortal();
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
    REQUIRE(std::string(TestLogText()).find("reason=malformed") != std::string::npos);
}

TEST_CASE("vector S: a 97-byte body is refused as malformed before parsing", "[T-4][FR-15]")
{
    StartPortal();
    std::string body(97, '1');   // any 97-byte content; the cap is checked before parsing
    Post(body);

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
}

TEST_CASE("vector S: the largest legal 59-byte body (four digits, leading zero) is accepted", "[T-4][FR-14][FR-15]")
{
    StartPortal();
    // Changed 2026-10-03: the former 1200/2000 for both bits has equal duty and now fails V4 (FR-14 note); vector X
    // with a leading zero on rst_us is the 59-byte valid body.
    const std::string body = "b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=0800";
    REQUIRE(body.size() == 59);   // FR-14's largest-accepted example: four digits, with a leading zero, in every value
    Post(body);
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Sent");
}

TEST_CASE("an empty body (Content-Length 0) is refused as malformed", "[T-4][FR-15]")
{
    StartPortal();
    Post("");
    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
}

TEST_CASE("GET /tuner serves the page at Debug level, never Info", "[T-4][FR-21]")
{
    StartPortal();
    REQUIRE(Get() == ESP_OK);

    REQUIRE(Status() == "200 OK");
    REQUIRE(std::string(TestHttpdContentType()) == "text/html");
    REQUIRE(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
    REQUIRE(TestLogCount(LOG_LEVEL_DEBUG) >= 1);
    REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
    REQUIRE(std::string(TestLogText()).find("serving tuner page") != std::string::npos);
}

// ---- T-4: no side effect other than the log line and the HTTP response --------------------------------------------

TEST_CASE("a valid tuner submission calls neither the scan nor the submit owner service", "[T-4][FR-19][FR-20]")
{
    StartPortal();
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(Status() == "200 OK");
    REQUIRE(g_scan_calls == 0);
    REQUIRE(g_submit_calls == 0);
    REQUIRE(TestNvsOpCount() == 0);
}

TEST_CASE("an invalid tuner submission calls neither the scan nor the submit owner service", "[T-4][FR-19][FR-20]")
{
    StartPortal();
    Post("b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");     // out_of_range
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280");   // bad_combination
    Post("not a valid body");                                            // malformed

    REQUIRE(g_scan_calls == 0);
    REQUIRE(g_submit_calls == 0);
    REQUIRE(TestNvsOpCount() == 0);
}

TEST_CASE("GET /tuner calls neither owner service and touches no NVS entry", "[T-4][FR-19][FR-20]")
{
    StartPortal();
    Get();

    REQUIRE(g_scan_calls == 0);
    REQUIRE(g_submit_calls == 0);
    REQUIRE(TestNvsOpCount() == 0);
}

TEST_CASE("a tuner submission does not change the SPEC-002 status endpoint", "[T-4][FR-20]")
{
    StartPortal();
    TestHttpdRequest(HTTP_GET, "/status", nullptr);
    REQUIRE(Body().empty());   // idle, nothing submitted to /submit

    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
    TestHttpdRequest(HTTP_GET, "/status", nullptr);
    REQUIRE(Body().empty());   // unaffected by the tuner request (FR-20)
}

TEST_CASE("static review: no NVS, GPIO, RMT/peripheral or provisioning-queue call in the tuner code", "[T-4][FR-19]")
{
    const std::string timing_source = ReadSource(WS2812_TIMING_SRC);
    const std::string portal_source = ReadSource(HTTP_PORTAL_SRC);

    const char *const kForbidden[] = {
        "nvs_", "gpio_", "rmt_", "ledc_", "spi_", "i2s_",
        "xQueueSend", "xQueueReceive", "provisioning.h", "esp_wifi",
    };
    for (const char *token : kForbidden) {
        INFO(token);
        REQUIRE(timing_source.find(token) == std::string::npos);
        REQUIRE(portal_source.find(token) == std::string::npos);
    }
}

TEST_CASE("static review: the timing set is cleared after every response", "[T-4][FR-19]")
{
    // Every branch of HandleTunerSubmitRequest() clears its local ws2812_timing_t before
    // returning, so no static/local copy of a submitted value outlives the response.
    const std::string portal_source = ReadSource(HTTP_PORTAL_SRC);
    size_t count = 0;
    for (size_t at = portal_source.find("memset(&timing, 0, sizeof(timing))"); at != std::string::npos;
         at = portal_source.find("memset(&timing, 0, sizeof(timing))", at + 1)) {
        ++count;
    }
    REQUIRE(count >= 3);   // out_of_range, bad_combination and the accepted path
    REQUIRE(portal_source.find("memset(s_tuner_body, 0, sizeof(s_tuner_body))") != std::string::npos);
}

// ---- T-12 (NFR-11): hostile input is rejected without reflection --------------------------------------------------

TEST_CASE("a <script> value is malformed and never reflected", "[T-12][NFR-11]")
{
    StartPortal();
    Post("b0h_ns=<script>alert(1)</script>&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
    REQUIRE(std::string(TestHttpdAllOutput()).find("script") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("script") == std::string::npos);
}

TEST_CASE("a percent-encoded %3C value is malformed and never reflected", "[T-12][NFR-11]")
{
    StartPortal();
    Post("b0h_ns=%3C&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
    REQUIRE(std::string(TestHttpdAllOutput()).find("%3C") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("%3C") == std::string::npos);
}

TEST_CASE("a control character in a value is malformed and never reflected", "[T-12][NFR-11]")
{
    StartPortal();
    std::string body = "b0h_ns=1";
    body += '\x01';
    body += "00&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280";
    Post(body);

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body() == "Invalid request");
    REQUIRE(std::string(TestHttpdAllOutput()).find('\x01') == std::string::npos);
}

TEST_CASE("a hostile extra key is ignored and never reflected in an accepted response", "[T-12][NFR-11]")
{
    StartPortal();
    Post("<script>alert(1)</script>=x&b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Sent");
    REQUIRE(std::string(TestHttpdAllOutput()).find("script") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("script") == std::string::npos);
}

TEST_CASE("only the fixed reject texts appear in a 400 response body, never client-supplied text", "[T-12][NFR-11]")
{
    StartPortal();
    const char *const kHostileBodies[] = {
        "b0h_ns=<img src=x onerror=alert(1)>&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",
        "b0h_ns=400\r\nSet-Cookie: x=1&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",
        "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280&../../etc/passwd=1",
    };
    for (const char *body : kHostileBodies) {
        INFO(body);
        Post(body);
        const std::string status = Status();
        const std::string response_body = Body();
        REQUIRE((response_body == "Sent" || response_body == "Invalid request" ||
                 response_body == "Value out of range" || response_body == "Invalid combination" ||
                 response_body == "Bit 0 duty must be less than bit 1 duty"));
    }
}

// ---- SPEC-004 T-4 (FR-4): the valid-submission hand-off to apply_led_timing -----------------------------------------

namespace {

struct AcceptedVector {
    const char *name;
    const char *body;
    ws2812_timing_t expected;
};

const AcceptedVector kAccepted[] = {
    {"A", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", {400, 1250, 800, 1250, 280}},
    // Changed 2026-10-03: B and D fail V4 and moved to kRejected; W, X, Y, AC and AD are the new valid vectors.
    {"C", "b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", {1075, 1200, 1100, 1200, 280}},
    {"E", "b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", {400, 1250, 800, 1250, 280}},
    {"T", "b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", {400, 1250, 800, 1250, 280}},
    {"U", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280&x=1", {400, 1250, 800, 1250, 280}},
    {"W", "b0h_ns=100&b0p_ns=800&b1h_ns=125&b1p_ns=800&rst_us=50", {100, 800, 125, 800, 50}},
    {"X", "b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800", {1000, 2000, 1200, 2000, 800}},
    {"Y", "b0h_ns=175&b0p_ns=1125&b1h_ns=125&b1p_ns=800&rst_us=280", {175, 1125, 125, 800, 280}},
    {"AC", "b0h_ns=500&b0p_ns=1250&b1h_ns=500&b1p_ns=1000&rst_us=280", {500, 1250, 500, 1000, 280}},
    {"AD", "b0h_ns=600&b0p_ns=2000&b1h_ns=500&b1p_ns=1000&rst_us=280", {600, 2000, 500, 1000, 280}},
};

const char *const kRejected[] = {
    "b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",      // F
    "b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",     // G
    "b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280",     // H
    "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40",      // I
    "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285",     // J
    "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810",     // K
    "b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280",    // L
    "b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280",    // M
    "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250",                // N
    "b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",     // O
    "b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",        // P
    "b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",    // Q
    "b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",   // R
    "",                                                             // S (Content-Length 0)
    "b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50",        // B  (V4, since 2026-10-03)
    "b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800",   // D  (V4, since 2026-10-03)
    "b0h_ns=125&b0p_ns=800&b1h_ns=175&b1p_ns=1125&rst_us=280",      // Z  (V4)
    "b0h_ns=400&b0p_ns=1000&b1h_ns=500&b1p_ns=1250&rst_us=280",     // AA (V4)
    "b0h_ns=500&b0p_ns=1000&b1h_ns=600&b1p_ns=1250&rst_us=280",     // AB (V4)
    "b0h_ns=1200&b0p_ns=1250&b1h_ns=400&b1p_ns=1250&rst_us=280",    // AE (V3 wins)
    "b0h_ns=1225&b0p_ns=2000&b1h_ns=400&b1p_ns=1250&rst_us=280",    // AF (range wins)
};

}  // namespace

TEST_CASE("a valid POST /tuner calls apply_led_timing exactly once with the parsed timing", "[SPEC-004][T-4][FR-4]")
{
    for (const AcceptedVector &vector : kAccepted) {
        DYNAMIC_SECTION("vector " << vector.name)
        {
            StartPortal();
            REQUIRE(Post(vector.body) == ESP_OK);

            REQUIRE(Status() == "200 OK");
            REQUIRE(Body() == "Sent");
            REQUIRE(FakeApplyLedTiming_fake.call_count == 1);
            REQUIRE(g_applied.size() == 1);
            const ws2812_timing_t &applied = g_applied[0].timing;
            REQUIRE(applied.bit0_high_ns == vector.expected.bit0_high_ns);
            REQUIRE(applied.bit0_period_ns == vector.expected.bit0_period_ns);
            REQUIRE(applied.bit1_high_ns == vector.expected.bit1_high_ns);
            REQUIRE(applied.bit1_period_ns == vector.expected.bit1_period_ns);
            REQUIRE(applied.reset_us == vector.expected.reset_us);
        }
    }
}

TEST_CASE("apply_led_timing gets the same values as the FR-17 log line", "[SPEC-004][T-4][FR-4]")
{
    StartPortal();
    Post("b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800");   // vector X (was D before V4)

    REQUIRE(g_applied.size() == 1);
    const ws2812_timing_t &t = g_applied[0].timing;
    char expected_line[128];
    std::snprintf(expected_line, sizeof(expected_line),
                  "tuner received: bit0 high_ns=%u period_ns=%u; bit1 high_ns=%u period_ns=%u; reset_us=%u",
                  t.bit0_high_ns, t.bit0_period_ns, t.bit1_high_ns, t.bit1_period_ns, t.reset_us);
    REQUIRE(std::string(TestLogText()).find(expected_line) != std::string::npos);
}

TEST_CASE("apply_led_timing is called after the log line and before the 200 Sent response", "[SPEC-004][T-4][FR-4]")
{
    StartPortal();
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");

    REQUIRE(g_applied.size() == 1);
    REQUIRE(g_applied[0].info_logs_at_call == 1);   // LogWs2812Timing() already ran
    REQUIRE(g_applied[0].body_at_call.empty());     // httpd_resp_send() not yet called
    REQUIRE(Body() == "Sent");
}

TEST_CASE("a rejected POST /tuner never calls apply_led_timing", "[SPEC-004][T-4][FR-4]")
{
    StartPortal();
    for (const char *body : kRejected) {
        INFO("body: " << body);
        Post(body);
        REQUIRE(Status() == "400 Bad Request");
    }
    Post(std::string(97, '1'));   // S: 97-byte body
    REQUIRE(Status() == "400 Bad Request");

    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
}

TEST_CASE("GET /tuner never calls apply_led_timing", "[SPEC-004][T-4][FR-4]")
{
    StartPortal();
    Get();
    REQUIRE(Status() == "200 OK");
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
}

TEST_CASE("each valid submission is handed off separately", "[SPEC-004][T-4][FR-4]")
{
    StartPortal();
    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
    Post("b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");   // rejected in between
    Post("b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800");   // vector X (was D before V4)

    REQUIRE(FakeApplyLedTiming_fake.call_count == 2);
    REQUIRE(g_applied[0].timing.bit0_high_ns == 400);
    REQUIRE(g_applied[1].timing.bit0_high_ns == 1000);
}


// =====================================================================================================================
// 2026-10-03 revision
// =====================================================================================================================

namespace {

const char *const kV4Text = "Bit 0 duty must be less than bit 1 duty";

struct HandlerVector {
    const char *id;
    std::string body;
    const char *status;
    const char *response;
    std::string log;   // the one expected log line (without the module prefix)
};

std::string Received(int b0h, int b0p, int b1h, int b1p, int rst)
{
    return "tuner received: bit0 high_ns=" + std::to_string(b0h) + " period_ns=" + std::to_string(b0p) +
           "; bit1 high_ns=" + std::to_string(b1h) + " period_ns=" + std::to_string(b1p) +
           "; reset_us=" + std::to_string(rst);
}

std::string Rejected(const char *reason) { return std::string("tuner request rejected: reason=") + reason; }

std::vector<HandlerVector> HandlerVectors()
{
    const std::string a = "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280";
    const std::string a_log = Received(400, 1250, 800, 1250, 280);
    const char *ok = "200 OK";
    const char *bad = "400 Bad Request";
    return {
        {"A", a, ok, "Sent", a_log},
        {"B", "b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50", bad, kV4Text, Rejected("bad_duty_order")},
        {"C", "b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280", ok, "Sent", Received(1075, 1200, 1100, 1200, 280)},
        {"D", "b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800", bad, kV4Text, Rejected("bad_duty_order")},
        {"E", "b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280", ok, "Sent", a_log},
        {"F", "b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Value out of range", Rejected("out_of_range")},
        {"G", "b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Value out of range", Rejected("out_of_range")},
        {"H", "b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Value out of range", Rejected("out_of_range")},
        {"I", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40", bad, "Value out of range", Rejected("out_of_range")},
        {"J", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285", bad, "Value out of range", Rejected("out_of_range")},
        {"K", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810", bad, "Value out of range", Rejected("out_of_range")},
        {"L", "b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", bad, "Invalid combination", Rejected("bad_combination")},
        {"M", "b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Invalid combination", Rejected("bad_combination")},
        {"N", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250", bad, "Invalid request", Rejected("malformed")},
        {"O", "b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Invalid request", Rejected("malformed")},
        {"P", "b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Invalid request", Rejected("malformed")},
        {"Q", "b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Invalid request", Rejected("malformed")},
        {"R", "b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", bad, "Invalid request", Rejected("malformed")},
        {"S", a + "&pad=" + std::string(TUNER_BODY_MAX + 1 - 56 - 5, 'x'), bad, "Invalid request", Rejected("malformed")},
        {"T", "b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280", ok, "Sent", a_log},
        {"U", a + "&x=1", ok, "Sent", a_log},
        {"V", "b0h_ns=90&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280", bad, "Value out of range", Rejected("out_of_range")},
        {"W", "b0h_ns=100&b0p_ns=800&b1h_ns=125&b1p_ns=800&rst_us=50", ok, "Sent", Received(100, 800, 125, 800, 50)},
        {"X", "b0h_ns=1000&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800", ok, "Sent", Received(1000, 2000, 1200, 2000, 800)},
        {"Y", "b0h_ns=175&b0p_ns=1125&b1h_ns=125&b1p_ns=800&rst_us=280", ok, "Sent", Received(175, 1125, 125, 800, 280)},
        {"Z", "b0h_ns=125&b0p_ns=800&b1h_ns=175&b1p_ns=1125&rst_us=280", bad, kV4Text, Rejected("bad_duty_order")},
        {"AA", "b0h_ns=400&b0p_ns=1000&b1h_ns=500&b1p_ns=1250&rst_us=280", bad, kV4Text, Rejected("bad_duty_order")},
        {"AB", "b0h_ns=500&b0p_ns=1000&b1h_ns=600&b1p_ns=1250&rst_us=280", bad, kV4Text, Rejected("bad_duty_order")},
        {"AC", "b0h_ns=500&b0p_ns=1250&b1h_ns=500&b1p_ns=1000&rst_us=280", ok, "Sent", Received(500, 1250, 500, 1000, 280)},
        {"AD", "b0h_ns=600&b0p_ns=2000&b1h_ns=500&b1p_ns=1000&rst_us=280", ok, "Sent", Received(600, 2000, 500, 1000, 280)},
        {"AE", "b0h_ns=1200&b0p_ns=1250&b1h_ns=400&b1p_ns=1250&rst_us=280", bad, "Invalid combination", Rejected("bad_combination")},
        {"AF", "b0h_ns=1225&b0p_ns=2000&b1h_ns=400&b1p_ns=1250&rst_us=280", bad, "Value out of range", Rejected("out_of_range")},
    };
}

std::string Header(const char *name)
{
    const char *value = TestHttpdHeader(name);
    return value == nullptr ? std::string("<none>") : std::string(value);
}

esp_err_t GetResult(const std::string &query_with_question_mark)
{
    return TestHttpdRequest(HTTP_GET, ("/tuner/result" + query_with_question_mark).c_str(), nullptr);
}

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

void PostValid(int times)
{
    for (int index = 0; index < times; ++index) {
        Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
        REQUIRE(Status() == "200 OK");
    }
}

bool HasCorsHeader()
{
    const std::string names = TestHttpdHeaderNames();
    return names.find("Access-Control") != std::string::npos || names.find("access-control") != std::string::npos;
}

int g_balance_at_send = -1000;
void RecordBalanceAtSend() { g_balance_at_send = MockGetMutexBalance(); }

}  // namespace

// ---- T-1 / T-15 / T-16: all 32 vectors through the POST /tuner handler -----------------------------------------------

TEST_CASE("all vectors A to AF through POST /tuner: status, body, header, one log line at the right level", "[T-1][T-6][T-15][T-16][FR-16][FR-17][FR-18][FR-22][FR-29]")
{
    const std::vector<HandlerVector> vectors = HandlerVectors();
    REQUIRE(vectors.size() == 32);
    for (const HandlerVector &vector : vectors) {
        INFO("vector " << vector.id);
        StartPortal();
        if (std::string(vector.id) == "S") {
            REQUIRE(vector.body.size() == TUNER_BODY_MAX + 1);
        }
        Post(vector.body);
        const bool accepted = std::string(vector.status) == "200 OK";
        REQUIRE(Status() == vector.status);
        REQUIRE(Body() == vector.response);
        REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
        REQUIRE(Header("Cache-Control") == "no-store");
        REQUIRE(Header("Tuner-Seq") == (accepted ? "1" : "<none>"));
        REQUIRE_FALSE(HasCorsHeader());
        const std::string level = accepted ? "[L1 http_portal] " : "[L2 http_portal] ";
        std::string expected_log = level + vector.log + "\n";
        if (accepted) {
            expected_log += "[L0 http_portal] tuner submission seq=1\n";   // FR-23 Debug line, after the Info line
        }
        REQUIRE(std::string(TestLogText()) == expected_log);
        REQUIRE(FakeApplyLedTiming_fake.call_count == (accepted ? 1u : 0u));
        REQUIRE(HarnessGetSubmitSeq() == (accepted ? 1u : 0u));
    }
}

TEST_CASE("a V4 rejection never calls apply_led_timing and never consumes a number", "[T-15][FR-19][FR-22][FR-23]")
{
    StartPortal();
    for (const char *id : {"B", "D", "Z", "AA", "AB"}) {
        for (const HandlerVector &vector : HandlerVectors()) {
            if (std::string(vector.id) != id) {
                continue;
            }
            INFO("vector " << id);
            TestLogReset();
            Post(vector.body);
            REQUIRE(Status() == "400 Bad Request");
            REQUIRE(Body() == kV4Text);
            REQUIRE(TestLogCount(LOG_LEVEL_INFO) == 0);
            REQUIRE(TestLogCount(LOG_LEVEL_WARNING) == 1);
            REQUIRE(std::string(TestLogText()).find("tuner submission seq") == std::string::npos);
        }
    }
    REQUIRE(FakeApplyLedTiming_fake.call_count == 0);
    REQUIRE(HarnessGetSubmitSeq() == 0);
    REQUIRE_FALSE(HarnessHasResultMutex());        // no measurement record touched (FR-19)
    REQUIRE(TestNvsOpCount() == 0);

    Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
    REQUIRE(Header("Tuner-Seq") == "1");           // the first valid submission still gets 1
}

// ---- T-16 (FR-17, FR-23): the submission sequence number ---------------------------------------------------------

TEST_CASE("valid POSTs get Tuner-Seq 1, 2, 3 with body Sent, and apply_led_timing gets the same number", "[T-16][FR-17][FR-23][SPEC-004][T-4][FR-4]")
{
    StartPortal();
    for (uint32_t expected = 1; expected <= 3; ++expected) {
        TestLogReset();
        Post("b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
        REQUIRE(Status() == "200 OK");
        REQUIRE(Body() == "Sent");
        REQUIRE(Header("Tuner-Seq") == std::to_string(expected));
        REQUIRE(g_applied.back().submit_seq == expected);
        REQUIRE(FakeApplyLedTiming_fake.arg1_val == expected);
        REQUIRE(std::string(TestLogText()).find("[L0 http_portal] tuner submission seq=" + std::to_string(expected) + "\n") !=
                std::string::npos);
    }
    REQUIRE(HarnessGetSubmitSeq() == 3);
}

TEST_CASE("the Tuner-Seq header is set before the body is sent and has no leading zeros", "[T-16][FR-17][FR-23]")
{
    StartPortal();
    PostValid(9);
    Post("b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280");   // vector E
    REQUIRE(Header("Tuner-Seq") == "10");
    REQUIRE(std::string(TestHttpdHeaderNames()) == "Cache-Control,Tuner-Seq");
    REQUIRE(g_applied.back().body_at_call.empty());                       // handed off before the response
}

TEST_CASE("rejected requests between valid ones leave the counter and send no header", "[T-16][FR-18][FR-23]")
{
    StartPortal();
    PostValid(1);
    for (const char *body : {"garbage", "b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280",
                             "b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280",
                             "b0h_ns=125&b0p_ns=800&b1h_ns=175&b1p_ns=1125&rst_us=280", ""}) {
        INFO(body);
        Post(body);
        REQUIRE(Status() == "400 Bad Request");
        REQUIRE(Header("Tuner-Seq") == "<none>");
        REQUIRE(HarnessGetSubmitSeq() == 1);
    }
    Post(std::string(97, '1'));
    REQUIRE(HarnessGetSubmitSeq() == 1);
    PostValid(1);
    REQUIRE(Header("Tuner-Seq") == "2");
    REQUIRE(g_applied.size() == 2);
    REQUIRE(g_applied[1].submit_seq == 2);
}

TEST_CASE("GET /tuner and GET /tuner/result never change the counter", "[T-16][FR-23][FR-25]")
{
    StartPortal();
    PostValid(2);
    Get();
    GetResult("?seq=1");
    GetResult("?seq=x");
    REQUIRE(HarnessGetSubmitSeq() == 2);
    PostValid(1);
    REQUIRE(Header("Tuner-Seq") == "3");
}

TEST_CASE("the counter survives a profile restart (not reset when a server starts or stops)", "[T-16][FR-23]")
{
    StartPortal();
    PostValid(2);
    StopHttpPortal();
    REQUIRE(StartHttpPortal(&kOps));
    PostValid(1);
    REQUIRE(Header("Tuner-Seq") == "3");
    REQUIRE(StartHttpStationServer(&kOps));   // the other profile shares the counter
    TestHttpdRequest(HTTP_POST, "/tuner", "b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280");
    REQUIRE(Header("Tuner-Seq") == "4");
}

// ---- T-17 / T-18 (FR-24 to FR-26, FR-3, FR-6): GET /tuner/result in the provisioning profile ----------------------

TEST_CASE("provisioning registers 19 handlers (limit 20) with GET /tuner/result before the catch-all", "[T-18][FR-3][FR-6]")
{
    StartPortal();
    REQUIRE(TestHttpdHandlerCount() == 19);              // SPEC-006 FR-7 (2026-10-04): + POST /tuner/read; was 18
    REQUIRE(TestHttpdConfig()->max_uri_handlers == 20);  // SPEC-006 FR-7: was 19
    int result_at = -1;
    int catch_all_at = -1;
    for (int index = 0; index < TestHttpdHandlerCount(); ++index) {
        const std::string uri = TestHttpdUriAt(index);
        if (uri == "/tuner/result") {
            result_at = index;
            REQUIRE(TestHttpdMethodAt(index) == HTTP_GET);
        }
        if (uri == "/*") {
            catch_all_at = index;
        }
    }
    REQUIRE(result_at >= 0);
    REQUIRE(catch_all_at == 18);   // SPEC-006 FR-7: was 17
    REQUIRE(result_at < catch_all_at);
}

TEST_CASE("provisioning: /tuner/result/ and POST /tuner/result go to the catch-all redirect", "[T-18][FR-3]")
{
    StartPortal();
    for (const auto &[method, uri] : std::vector<std::pair<int, const char *>>{
             {HTTP_GET, "/tuner/result/"}, {HTTP_POST, "/tuner/result"}, {HTTP_POST, "/tuner/result?seq=1"}}) {
        INFO(uri);
        TestHttpdRequest(method, uri, method == HTTP_POST ? "x=1" : nullptr);
        REQUIRE(Status() == "302 Found");
        REQUIRE(Header("Location") == HTTP_PORTAL_URL);
    }
    GetResult("?seq=0");
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "state=unknown");
}

TEST_CASE("GET /tuner/result: text/plain, no-store, no CORS header, and a well-formed poll logs nothing", "[T-17][FR-25][SPEC-005][FR-32]")
{
    StartPortal();
    PostValid(1);
    TestLogReset();
    for (const char *query : {"?seq=0", "?seq=1", "?seq=2", "?seq=4294967295", "?x=1&seq=1"}) {
        INFO(query);
        REQUIRE(GetResult(query) == ESP_OK);
        REQUIRE(Status() == "200 OK");
        REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
        REQUIRE(Header("Cache-Control") == "no-store");
        REQUIRE(std::string(TestHttpdHeaderNames()) == "Cache-Control");   // nothing else, no Access-Control-*
    }
    REQUIRE(std::string(TestLogText()).empty());
    REQUIRE(TestHttpdHeaderReadCount() == 0);   // no Origin (or any other header) is read
    REQUIRE(FakeApplyLedTiming_fake.call_count == 1);
}

TEST_CASE("GET /tuner/result: malformed requests get 400 Invalid request with one Debug line only", "[T-17][FR-25]")
{
    StartPortal();
    PostValid(1);
    const std::string q31 = "?x=" + std::string(23, 'a') + "&seq=1";   // 31-byte query: still well-formed
    const std::string q32 = "?x=" + std::string(24, 'a') + "&seq=1";   // 32-byte query: malformed
    REQUIRE(q31.size() - 1 == 31);
    REQUIRE(q32.size() - 1 == 32);
    for (const std::string &query : {std::string(""), std::string("?"), std::string("?seq="), std::string("?seq"),
                                     std::string("?seqx=1"), std::string("?seq=abc"), std::string("?seq=12345678901"),
                                     std::string("?seq=4294967296"), std::string("?x=1"), q32,
                                     "?seq=1&" + std::string(40, 'b')}) {
        INFO("query '" << query << "'");
        TestLogReset();
        GetResult(query);
        REQUIRE(Status() == "400 Bad Request");
        REQUIRE(Body() == "Invalid request");
        REQUIRE(std::string(TestHttpdContentType()) == "text/plain");
        REQUIRE(Header("Cache-Control") == "no-store");
        REQUIRE_FALSE(HasCorsHeader());
        REQUIRE(std::string(TestLogText()) == "[L0 http_portal] tuner result request malformed\n");
    }
    TestLogReset();
    GetResult(q31);
    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "state=pending");
    REQUIRE(std::string(TestLogText()).empty());
}

TEST_CASE("GET /tuner/result: malformed text is never reflected", "[T-17][FR-25][NFR-11]")
{
    StartPortal();
    GetResult("?seq=<script>");
    REQUIRE(Body() == "Invalid request");
    REQUIRE(std::string(TestHttpdAllOutput()).find("script") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("script") == std::string::npos);
}

TEST_CASE("GET /tuner/result serves every row of the section 7.6 table", "[T-17][FR-24][FR-25][FR-26]")
{
    StartPortal();
    PostValid(9);   // numbers 1..9 issued
    const auto body_for = [](const char *query) {
        GetResult(query);
        REQUIRE(Status() == "200 OK");
        return Body();
    };

    // Never set: the mutex does not exist yet; every issued number is pending.
    REQUIRE_FALSE(HarnessHasResultMutex());
    REQUIRE(body_for("?seq=5") == "state=pending");

    ws2812_measurement_t done = Measurement(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&done);
    REQUIRE(body_for("?seq=3") == "state=done&b0=400&b1=800&match=144");
    REQUIRE(body_for("?seq=2") == "state=superseded");
    REQUIRE(body_for("?seq=5") == "state=pending");
    REQUIRE(body_for("?seq=0") == "state=unknown");
    REQUIRE(body_for("?seq=99") == "state=unknown");
    REQUIRE(body_for("?seq=10") == "state=unknown");   // one beyond the last issued number

    ws2812_measurement_t ac = Measurement(4, WS2812_MEASUREMENT_DONE, 500, 500, 0, false);
    SetHttpTunerResult(&ac);
    REQUIRE(body_for("?seq=4") == "state=done&b0=500&b1=500&match=n/a");
    ws2812_measurement_t timeout = Measurement(6, WS2812_MEASUREMENT_TIMEOUT);
    SetHttpTunerResult(&timeout);
    REQUIRE(body_for("?seq=6") == "state=timeout");
    ws2812_measurement_t count_error = Measurement(7, WS2812_MEASUREMENT_COUNT_ERROR);
    SetHttpTunerResult(&count_error);
    REQUIRE(body_for("?seq=7") == "state=count_error");
    ws2812_measurement_t not_measured = Measurement(8, WS2812_MEASUREMENT_NOT_MEASURED);
    SetHttpTunerResult(&not_measured);
    REQUIRE(body_for("?seq=8") == "state=not_measured");
    // Number 9: skipped by led_controller before arming, nothing published -> pending ...
    REQUIRE(body_for("?seq=9") == "state=pending");
    REQUIRE(body_for("?seq=3") == "state=superseded");
    // ... until a newer outcome is stored.
    PostValid(1);
    ws2812_measurement_t newer = Measurement(10, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&newer);
    REQUIRE(body_for("?seq=9") == "state=superseded");
    REQUIRE(body_for("?seq=10") == "state=done&b0=400&b1=800&match=144");
}

TEST_CASE("GET /tuner/result after a reboot: numbers from before are unknown", "[T-17][FR-26]")
{
    StartPortal();
    PostValid(3);
    ws2812_measurement_t done = Measurement(3, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&done);
    StartPortal();   // resets the http_portal statics, like a reboot
    for (const char *query : {"?seq=1", "?seq=2", "?seq=3"}) {
        GetResult(query);
        REQUIRE(Body() == "state=unknown");
    }
}

TEST_CASE("GET /tuner/result reads the record under the mutex, once, and responds after releasing it", "[T-17][FR-24][NFR-7][NFR-20]")
{
    StartPortal();
    PostValid(1);
    GetResult("?seq=1");                              // no mutex yet: no take, no crash
    REQUIRE(Body() == "state=pending");
    REQUIRE(MockGetMutexBalance() == 0);

    ws2812_measurement_t done = Measurement(1, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&done);                        // creates the mutex
    REQUIRE(HarnessHasResultMutex());
    const int takes = HarnessGetResultMutexTakes();
    const int gives = HarnessGetResultMutexGives();

    g_balance_at_send = -1000;
    TestHttpdSetSendHook(RecordBalanceAtSend);
    GetResult("?seq=1");
    TestHttpdSetSendHook(nullptr);
    REQUIRE(Body() == "state=done&b0=400&b1=800&match=144");
    REQUIRE(HarnessGetResultMutexTakes() == takes + 1);
    REQUIRE(HarnessGetResultMutexGives() == gives + 1);
    REQUIRE(g_balance_at_send == 0);                  // formatted and sent after the give

    GetResult("?seq=abc");                            // malformed: the record is not read
    REQUIRE(HarnessGetResultMutexTakes() == takes + 1);
    REQUIRE(MockGetMutexBalance() == 0);
}

// ---- FR-24: SetHttpTunerResult() --------------------------------------------------------------------------------

TEST_CASE("SetHttpTunerResult creates the mutex once, stores under it, and never moves backwards", "[T-17][FR-24]")
{
    StartPortal();
    REQUIRE_FALSE(HarnessHasResultMutex());
    SetHttpTunerResult(nullptr);                      // ignored, creates nothing
    REQUIRE_FALSE(HarnessHasResultMutex());

    ws2812_measurement_t five = Measurement(5, WS2812_MEASUREMENT_TIMEOUT);
    SetHttpTunerResult(&five);
    REQUIRE(HarnessHasResultMutex());
    REQUIRE(HarnessGetResultMutexTakes() == 1);
    REQUIRE(HarnessGetResultMutexGives() == 1);
    REQUIRE(HarnessGetTunerResult().submit_seq == 5);

    ws2812_measurement_t four = Measurement(4, WS2812_MEASUREMENT_DONE, 1, 2, 3, true);
    SetHttpTunerResult(&four);                        // lower: ignored
    REQUIRE(HarnessGetTunerResult().submit_seq == 5);
    REQUIRE(HarnessGetTunerResult().state == WS2812_MEASUREMENT_TIMEOUT);
    REQUIRE(HarnessGetResultMutexTakes() == 2);       // still taken and given (same mutex, not re-created)
    REQUIRE(HarnessGetResultMutexGives() == 2);

    ws2812_measurement_t five_done = Measurement(5, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&five_done);                   // equal: stored (>=), two producers for one number
    REQUIRE(HarnessGetTunerResult().state == WS2812_MEASUREMENT_DONE);
    REQUIRE(HarnessGetTunerResult().bit1_high_avg_ns == 800);

    ws2812_measurement_t zero = Measurement(0, WS2812_MEASUREMENT_DONE);
    SetHttpTunerResult(&zero);                        // boot frame after a newer one: ignored
    REQUIRE(HarnessGetTunerResult().submit_seq == 5);

    ws2812_measurement_t max = Measurement(4294967295u, WS2812_MEASUREMENT_NOT_MEASURED);
    SetHttpTunerResult(&max);
    REQUIRE(HarnessGetTunerResult().submit_seq == 4294967295u);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("an older outcome published after a newer one does not regress the served state", "[T-17][FR-24][FR-26]")
{
    StartPortal();
    PostValid(2);
    ws2812_measurement_t newer = Measurement(2, WS2812_MEASUREMENT_NOT_MEASURED);
    ws2812_measurement_t older = Measurement(1, WS2812_MEASUREMENT_DONE, 400, 800, 144, true);
    SetHttpTunerResult(&newer);
    SetHttpTunerResult(&older);
    GetResult("?seq=2");
    REQUIRE(Body() == "state=not_measured");
    GetResult("?seq=1");
    REQUIRE(Body() == "state=superseded");
}
