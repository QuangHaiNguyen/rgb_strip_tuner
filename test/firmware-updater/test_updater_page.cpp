/**
 * @file test_updater_page.cpp
 * @brief SPEC-007 T-7 (static part) and FR-23: the updater page as GET / serves it (updater/main/updater_page.c through
 *        updater_http.c): at most 3,072 bytes for every reason and version, exactly one file input and one Upload
 *        button, no external URL, the FR-24 texts and the FR-26 reason lines; the handler set (GET /, POST /update,
 *        GET /status, 404 Not found) and the server configuration (port 80, stack <= 6,144 bytes, NFR-3).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <regex>
#include <string>

extern "C" {
#include "fw_flash_fakes.h"
#include "host_stubs.h"
#include "httpd_mock.h"
#include "updater_http.h"
#include "updater_http_harness.h"
#include "updater_page.h"
}

namespace {

int Count(const std::string &text, const std::string &needle)
{
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

std::string GetPage(updater_reason_t reason, const char *installed)
{
    TestFlashReset();
    TestLogReset();
    REQUIRE(HarnessStartUpdaterHttp(reason, installed));
    (void)TestHttpdRequest(HTTP_GET, "/", nullptr);
    REQUIRE(HarnessChunkTerminated());
    return std::string(HarnessChunkBody(), HarnessChunkBodyLength());
}

struct ReasonCase {
    updater_reason_t reason;
    const char *name;
    const char *text;
};

const ReasonCase kReasons[] = {
    {UPDATER_REASON_REQUESTED, "requested", "Update requested from the device."},
    {UPDATER_REASON_NO_FIRMWARE, "no_firmware", "No valid firmware installed."},
    {UPDATER_REASON_CRASH_LOOP, "crash_loop", "The firmware crashed repeatedly and was stopped."},
    {UPDATER_REASON_BOOT_SELECT_FAILED, "boot_select_failed", "Could not start the firmware."},
};

}  // namespace

TEST_CASE("FR-26 reason texts and FR-7 log tokens come from a fixed table", "[T-7][FR-7][FR-12][FR-26]")
{
    for (const ReasonCase &reason : kReasons) {
        CAPTURE(reason.name);
        CHECK(std::string(GetUpdaterReasonText(reason.reason)) == reason.text);
        CHECK(std::string(GetUpdaterReasonName(reason.reason)) == reason.name);
    }
}

TEST_CASE("GET / is at most 3,072 bytes for every reason, with and without an installed version", "[T-7][FR-24]")
{
    CHECK(GetUpdaterPageMaxBytes() <= (size_t)UPDATER_PAGE_MAX_BYTES);
    CHECK(UPDATER_PAGE_MAX_BYTES == 3072);
    for (const ReasonCase &reason : kReasons) {
        for (const char *installed : {(const char *)nullptr, "99.99.99"}) {
            CAPTURE(reason.name, installed == nullptr ? "none" : installed);
            std::string page = GetPage(reason.reason, installed);
            CHECK(page.size() <= 3072u);
            CHECK(page.size() <= GetUpdaterPageMaxBytes());
        }
    }
}

TEST_CASE("GET / headers: 200, text/html, Cache-Control no-store", "[T-7][FR-24]")
{
    (void)GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(std::string(TestHttpdStatus()) == "200 OK");
    CHECK(std::string(TestHttpdContentType()) == "text/html");
    REQUIRE(TestHttpdHeader("Cache-Control") != nullptr);
    CHECK(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
}

TEST_CASE("the page has the heading, installed line, reason line, label and status region", "[T-7][FR-24][FR-26]")
{
    const ReasonCase &reason = GENERATE(from_range(std::begin(kReasons), std::end(kReasons)));
    CAPTURE(reason.name);
    std::string page = GetPage(reason.reason, "01.02.03");
    CHECK(Count(page, "<h1>RGB LED Tuner firmware update</h1>") == 1);
    CHECK(Count(page, "Installed firmware: 01.02.03") == 1);
    CHECK(Count(page, reason.text) == 1);
    for (const ReasonCase &other : kReasons) {
        if (other.reason != reason.reason) {
            CHECK(Count(page, other.text) == 0);
        }
    }
    CHECK(Count(page, "Firmware file (.bin)") == 1);
    CHECK(Count(page, "role=\"status\"") == 1);
}

TEST_CASE("no installed firmware -> 'Installed firmware: none'", "[T-7][FR-24]")
{
    std::string page = GetPage(UPDATER_REASON_NO_FIRMWARE, nullptr);
    CHECK(Count(page, "Installed firmware: none") == 1);
}

TEST_CASE("exactly one file input (accept .bin) and exactly one Upload button", "[T-7][FR-24]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(Count(page, "<input") == 1);
    CHECK(Count(page, "type=\"file\"") == 1);
    CHECK(Count(page, "accept=\".bin\"") == 1);
    CHECK(Count(page, "<button") == 1);
    CHECK(std::regex_search(page, std::regex("<button[^>]*>Upload</button>")));
    CHECK(Count(page, "<form") == 0);
}

TEST_CASE("the page has no external reference", "[T-7][FR-24][NFR-7]")
{
    std::string page = GetPage(UPDATER_REASON_CRASH_LOOP, "01.00.00");
    CHECK(Count(page, "http://") == 0);
    CHECK(Count(page, "https://") == 0);
    CHECK(Count(page, "src=") == 0);
    CHECK(Count(page, "href=") == 0);
    CHECK(Count(page, "url(") == 0);
    CHECK(Count(page, "@import") == 0);
    CHECK_FALSE(std::regex_search(page, std::regex("['\"]//")));   /* protocol-relative URL */
}

TEST_CASE("the page script posts the raw file to /update as application/octet-stream with the FR-25 texts",
          "[T-7][FR-25]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(Count(page, "'POST','/update'") == 1);
    CHECK(Count(page, "application/octet-stream") == 1);
    CHECK(Count(page, "multipart") == 0);
    CHECK(Count(page, "Select a .bin file") == 1);
    CHECK(Count(page, "Not a .bin file") == 1);
    CHECK(Count(page, "Uploading... ") >= 1);
    CHECK(std::regex_search(page, std::regex(R"(/\\\.bin\$/i)")));   /* case-insensitive .bin check */
}

TEST_CASE("exactly three URI handlers plus the 404 handler; port 80; stack <= 6,144 bytes", "[T-7][FR-23][NFR-3]")
{
    TestFlashReset();
    TestLogReset();
    REQUIRE(HarnessStartUpdaterHttp(UPDATER_REASON_REQUESTED, nullptr));
    REQUIRE(TestHttpdHandlerCount() == 3);
    CHECK(std::string(TestHttpdUriAt(0)) == "/");
    CHECK(TestHttpdMethodAt(0) == HTTP_GET);
    CHECK(std::string(TestHttpdUriAt(1)) == "/update");
    CHECK(TestHttpdMethodAt(1) == HTTP_POST);
    CHECK(std::string(TestHttpdUriAt(2)) == "/status");
    CHECK(TestHttpdMethodAt(2) == HTTP_GET);
    CHECK(TestHttpdErrHandler(HTTPD_404_NOT_FOUND) != nullptr);
    CHECK(TestHttpdConfig()->server_port == 80);
    CHECK(TestHttpdConfig()->stack_size <= 6144u);
}

TEST_CASE("any other path -> 404, text/plain, body 'Not found'", "[T-7][FR-23]")
{
    TestFlashReset();
    REQUIRE(HarnessStartUpdaterHttp(UPDATER_REASON_REQUESTED, nullptr));
    for (const char *uri : {"/index.html", "/generate_204", "/update/x", "/statusx"}) {
        CAPTURE(uri);
        (void)TestHttpdRequest(HTTP_GET, uri, nullptr);
        CHECK(std::string(TestHttpdStatus()) == "404 Not Found");
        CHECK(std::string(TestHttpdContentType()) == "text/plain");
        CHECK(std::string(TestHttpdBody()) == "Not found");
    }
}

TEST_CASE("StartUpdaterHttp fails cleanly if the server or a handler cannot start", "[T-7][FR-23]")
{
    TestFlashReset();
    TestLogReset();
    TestHttpdReset();
    TestHttpdFailStart(true);
    CHECK_FALSE(StartUpdaterHttp(UPDATER_REASON_REQUESTED, TestOtaPartition(), nullptr));
    TestHttpdFailStart(false);
    TestHttpdFailRegistrationAt(1);
    CHECK_FALSE(StartUpdaterHttp(UPDATER_REASON_REQUESTED, TestOtaPartition(), nullptr));
    CHECK(TestHttpdStopCount() == 1);
    CHECK(TestLogCount(3) == 2);
    TestHttpdFailRegistrationAt(-1);
}

/* ---- T-19 / FR-25 (section 0.8): GET /status pre-check before POST /update (static part; behavior in page_js/) ---- */

TEST_CASE("the page requests GET /status before POST /update, and only posts from the /status answer", "[T-19][FR-25]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    const size_t script_at = page.find("<script>");
    REQUIRE(script_at != std::string::npos);
    const std::string script = page.substr(script_at);
    CHECK(Count(script, "'GET','/status'") == 1);
    CHECK(Count(script, "'POST','/update'") == 1);
    /* The click handler opens /status; the POST helper is only called from the /status onload. */
    const size_t click_at = script.find("u.onclick=");
    REQUIRE(click_at != std::string::npos);
    const std::string click = script.substr(click_at);
    CHECK(click.find("'GET','/status'") != std::string::npos);
    CHECK(click.find("'POST','/update'") == std::string::npos);
    CHECK(std::regex_search(click, std::regex(R"(q\.onload=function\(\)\{[^}]*else p\(f\))")));
    CHECK(Count(click, "p(f)") == 1);
}

TEST_CASE("the page has the FR-25 pre-check texts and checks state=uploading|done", "[T-19][FR-25]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(Count(page, "'Upload in progress'") == 1);
    CHECK(Count(page, "'Send failed, check connection'") == 2);   /* non-200 and request error */
    CHECK(Count(page, "state=(uploading|done)") == 1);
    CHECK(Count(page, "status!=200") == 1);
}

TEST_CASE("the page uses no timer and no automatic retry", "[T-19][FR-25]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(Count(page, "setTimeout") == 0);
    CHECK(Count(page, "setInterval") == 0);
    CHECK(Count(page, "fetch(") == 0);
    CHECK(Count(page, ".timeout") == 0);
}

TEST_CASE("the page with the pre-check still fits 3,072 bytes", "[T-19][FR-24][FR-25]")
{
    CHECK(GetUpdaterPageMaxBytes() <= 3072u);
    std::string page = GetPage(UPDATER_REASON_CRASH_LOOP, "99.99.99");   /* the longest reason text */
    CHECK(page.size() <= 3072u);
}

TEST_CASE("section 0.9: the page refuses a file above 1,114,112 bytes before the /status pre-check", "[T-19][FR-25]")
{
    std::string page = GetPage(UPDATER_REASON_REQUESTED, "01.00.00");
    size_t size_check = page.find("if(f.size>1114112){s.textContent='Invalid size';return;}");
    REQUIRE(size_check != std::string::npos);
    CHECK(size_check < page.find("/status"));
}
