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
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
#include "freertos_mock.h"
#include "http_portal.h"
#include "httpd_mock.h"
#include "log_fakes.h"
#include "logging.h"
#include "nvs_fake.h"
#include "ws2812_timing.h"
void HarnessResetHttpPortal(void);
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

const http_portal_ops_t kOps = {FakeScan, FakeSubmit};

void StartPortal()
{
    MockFreeRtosReset();
    TestHttpdReset();
    TestNvsReset();
    HarnessResetHttpPortal();
    g_scan_calls = 0;
    g_submit_calls = 0;
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
    const std::string body = "b0h_ns=1200&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=0800";
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
                 response_body == "Value out of range" || response_body == "Invalid combination"));
    }
}
