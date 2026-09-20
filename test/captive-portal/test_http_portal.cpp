/**
 * @file test_http_portal.cpp
 * @brief Host tests for the captive-portal HTTP server (SPEC-002 T-4, T-5, T-6, T-7; FR-10..FR-14, FR-19, FR-22).
 */
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "freertos_mock.h"
#include "host_stubs.h"
#include "http_portal.h"
#include "httpd_mock.h"
void HarnessResetHttpPortal(void);
}

namespace {

// ---- Owner services faked for the portal ---------------------------------------------------------------------------
std::vector<wifi_scan_entry_t> g_scan_entries;
bool g_scan_fails = false;
bool g_submit_accepts = true;
std::vector<wifi_credentials_t> g_submissions;
int g_scan_calls = 0;

int FakeScan(wifi_scan_entry_t *entries, uint16_t max_entries)
{
    ++g_scan_calls;
    if (g_scan_fails) {
        return -1;
    }
    size_t count = std::min<size_t>(g_scan_entries.size(), max_entries);
    std::memcpy(entries, g_scan_entries.data(), count * sizeof(entries[0]));
    return static_cast<int>(count);
}

bool FakeSubmit(const wifi_credentials_t *credentials)
{
    g_submissions.push_back(*credentials);
    return g_submit_accepts;
}

const http_portal_ops_t kOps = {FakeScan, FakeSubmit};

const char *const kProbeUris[] = {
    "/generate_204", "/gen_204",
    "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/redirect",
    "/canonical.html", "/success.txt", "/check_network_status.txt",
};

wifi_scan_entry_t Entry(const std::string &ssid, int rssi_dbm, bool open = false, bool unsupported = false)
{
    wifi_scan_entry_t entry = {};
    std::strncpy(entry.ssid, ssid.c_str(), sizeof(entry.ssid) - 1);
    entry.rssi_dbm = static_cast<int8_t>(rssi_dbm);
    entry.is_open = open;
    entry.is_unsupported = unsupported;
    return entry;
}

void StartPortal()
{
    MockFreeRtosReset();
    TestHttpdReset();
    TestLogReset();
    HarnessResetHttpPortal();
    g_scan_entries.clear();
    g_scan_fails = false;
    g_submit_accepts = true;
    g_submissions.clear();
    g_scan_calls = 0;
    REQUIRE(StartHttpPortal(&kOps));
}

std::string Body() { return TestHttpdBody(); }
std::string Status() { return TestHttpdStatus(); }

/** Submit a form after a scan that lists the given networks. */
void Submit(const std::string &form)
{
    TestHttpdRequest(HTTP_POST, "/submit", form.c_str());
}

std::string PollStatus()
{
    TestHttpdRequest(HTTP_GET, "/status", nullptr);
    return Body();
}

}  // namespace

// ---- Server setup ---------------------------------------------------------------------------------------------------

TEST_CASE("the server registers every connectivity-check URL and a trailing wildcard", "[T-4][FR-10]")
{
    StartPortal();

    for (const char *uri : {"/", "/scan", "/submit", "/status"}) {
        INFO(uri);
        REQUIRE(TestHttpdHasUri(uri));
    }
    for (const char *uri : kProbeUris) {
        INFO(uri);
        REQUIRE(TestHttpdHasUri(uri));
    }
    const int count = TestHttpdHandlerCount();
    REQUIRE(std::string(TestHttpdUriAt(count - 1)) == "/*");          // exact URIs must win over the wildcard
    REQUIRE(count <= TestHttpdConfig()->max_uri_handlers);
    REQUIRE(TestHttpdMethodAt(count - 1) == HTTP_ANY);
}

TEST_CASE("the server is configured for wildcard matching and few sockets", "[T-8][FR-10][NFR-7]")
{
    StartPortal();
    const httpd_config_t *config = TestHttpdConfig();
    REQUIRE(config->uri_match_fn == httpd_uri_match_wildcard);
    REQUIRE(config->lru_purge_enable);
    REQUIRE(config->max_open_sockets <= 7);
    REQUIRE(config->stack_size >= 4096);
}

TEST_CASE("start, restart and stop manage a single server", "[T-8][FR-20][NFR-7]")
{
    StartPortal();
    REQUIRE(TestHttpdStartCount() == 1);
    REQUIRE(TestHttpdStopCount() == 0);

    REQUIRE(StartHttpPortal(&kOps));                // restart without a stop must not leak the old server
    REQUIRE(TestHttpdStartCount() == 2);
    REQUIRE(TestHttpdStopCount() == 1);

    StopHttpPortal();
    StopHttpPortal();                               // second stop is a no-op
    REQUIRE(TestHttpdStopCount() == 2);
}

TEST_CASE("start fails cleanly without ops or when the server cannot start", "[T-8][FR-20]")
{
    TestHttpdReset();
    HarnessResetHttpPortal();
    REQUIRE_FALSE(StartHttpPortal(nullptr));
    REQUIRE(TestHttpdStartCount() == 0);

    TestHttpdFailStart(true);
    REQUIRE_FALSE(StartHttpPortal(&kOps));
    REQUIRE(TestHttpdHandlerCount() == 0);
    StopHttpPortal();                               // nothing running
    REQUIRE(TestHttpdStopCount() == 0);
}

// ---- Captive redirects ----------------------------------------------------------------------------------------------

TEST_CASE("connectivity probes are redirected to the portal", "[T-4][FR-10]")
{
    StartPortal();
    for (const char *uri : kProbeUris) {
        INFO(uri);
        TestHttpdRequest(HTTP_GET, uri, nullptr);
        REQUIRE(Status() == "302 Found");
        REQUIRE(std::string(TestHttpdHeader("Location")) == "http://192.168.4.1/");
    }
}

TEST_CASE("arbitrary hosts, paths and methods reach the portal", "[T-4][FR-10]")
{
    StartPortal();
    const struct { int method; const char *uri; } requests[] = {
        {HTTP_GET, "/anything"}, {HTTP_GET, "/a/b/c.html?x=1&y=2"}, {HTTP_GET, "/favicon.ico"},
        {HTTP_POST, "/whatever"}, {HTTP_GET, "/generate_204?ts=1"}, {HTTP_POST, "/generate_204"},
    };
    for (const auto &request : requests) {
        INFO(request.uri);
        TestHttpdRequest(request.method, request.uri, nullptr);
        REQUIRE(Status() == "302 Found");
        REQUIRE(std::string(TestHttpdHeader("Location")) == HTTP_PORTAL_URL);
    }
}

TEST_CASE("the root serves the provisioning page", "[T-4][T-5][FR-11][FR-12][FR-13]")
{
    StartPortal();
    TestHttpdRequest(HTTP_GET, "/", nullptr);

    const std::string page = Body();
    REQUIRE(Status() == "200 OK");
    REQUIRE(std::string(TestHttpdContentType()) == "text/html");
    REQUIRE(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
    REQUIRE(page.find("Refresh") != std::string::npos);                    // FR-12
    REQUIRE(page.find("type=password") != std::string::npos);              // FR-13: masked input
    REQUIRE(page.find("/scan") != std::string::npos);
    REQUIRE(page.find("/submit") != std::string::npos);
    REQUIRE(page.find("/status") != std::string::npos);                    // FR-22: page polls the result
    REQUIRE(page.find("Scanning...") != std::string::npos);                // FR-12: progress indication
    REQUIRE(page.find("o.disabled=x.uns") != std::string::npos);           // FR-13: enterprise not selectable
    REQUIRE(page.find("new Option(") != std::string::npos);                // SSIDs go in as text, not markup
}

// ---- Scan endpoint --------------------------------------------------------------------------------------------------

TEST_CASE("the scan endpoint lists networks in the order given, with dBm and flags", "[T-5][FR-11]")
{
    StartPortal();
    g_scan_entries = {Entry("cafe", -40, true), Entry("corp", -55, false, true), Entry("home", -70)};

    TestHttpdRequest(HTTP_GET, "/scan?ts=123", nullptr);

    REQUIRE(Status() == "200 OK");
    REQUIRE(std::string(TestHttpdContentType()) == "application/json");
    REQUIRE(Body() == "[{\"ssid\":\"cafe\",\"rssi\":-40,\"open\":true,\"uns\":false},"
                      "{\"ssid\":\"corp\",\"rssi\":-55,\"open\":false,\"uns\":true},"
                      "{\"ssid\":\"home\",\"rssi\":-70,\"open\":false,\"uns\":false}]");
    REQUIRE(g_scan_calls == 1);
}

TEST_CASE("an empty scan is an empty list", "[T-5][FR-11]")
{
    StartPortal();
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);
    REQUIRE(Body() == "[]");
}

TEST_CASE("twenty networks fit in one response", "[T-5][FR-11]")
{
    StartPortal();
    for (int index = 0; index < 20; ++index) {
        g_scan_entries.push_back(Entry("network-number-" + std::to_string(index), -30 - index));
    }
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    const std::string body = Body();
    int entries = 0;
    for (size_t at = body.find("{\"ssid\""); at != std::string::npos; at = body.find("{\"ssid\"", at + 1)) {
        ++entries;
    }
    REQUIRE(entries == 20);
}

TEST_CASE("a hostile SSID cannot break the JSON or inject markup", "[T-5][FR-11]")
{
    StartPortal();
    g_scan_entries = {Entry("\"}],\"x\":[<script>alert(1)</script>&'\n", -40)};

    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    const std::string body = Body();
    REQUIRE(body.find('<') == std::string::npos);
    REQUIRE(body.find('>') == std::string::npos);
    REQUIRE(body.find('&') == std::string::npos);
    REQUIRE(body.find('\'') == std::string::npos);
    REQUIRE(body.find('\n') == std::string::npos);
    REQUIRE(body.find("\\u003cscript\\u003e") != std::string::npos);
    REQUIRE(body.find("\\\"}],\\\"x\\\"") != std::string::npos);        // the quotes stay inside the string
    REQUIRE(body.substr(0, 10) == "[{\"ssid\":\"");
    REQUIRE(body.substr(body.size() - 2) == "}]");
}

TEST_CASE("a response larger than the buffer is cut, never overrun", "[T-5][FR-11]")
{
    StartPortal();
    for (int index = 0; index < 20; ++index) {
        g_scan_entries.push_back(Entry(std::string(32, '\x01'), -30 - index));   // 6 output bytes per input byte
    }
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    const std::string body = Body();
    REQUIRE(body.size() < 4096);
    REQUIRE(body.front() == '[');
    REQUIRE(body.substr(body.size() - 2) == "}]");                               // still well formed
    const auto count = [&](char c) { return std::count(body.begin(), body.end(), c); };
    REQUIRE(count('{') == count('}'));
}

TEST_CASE("a scan that cannot run answers 503 and keeps the previous list", "[T-5][FR-12]")
{
    StartPortal();
    g_scan_entries = {Entry("home", -50)};
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);
    REQUIRE(Status() == "200 OK");

    g_scan_fails = true;
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);
    REQUIRE(Status() == "503 Service Unavailable");
    REQUIRE(Body().find('[') == std::string::npos);                              // not JSON: the page keeps its list
}

// ---- Submission -----------------------------------------------------------------------------------------------------

TEST_CASE("a valid secured submission starts a connection trial", "[T-6][FR-14][FR-15][FR-22]")
{
    StartPortal();
    g_scan_entries = {Entry("home", -50)};
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    Submit("ssid=home&password=correct+horse");

    REQUIRE(Status() == "200 OK");
    REQUIRE(Body() == "Connecting...");
    REQUIRE(g_submissions.size() == 1);
    REQUIRE(std::string(g_submissions[0].ssid) == "home");
    REQUIRE(std::string(g_submissions[0].password) == "correct horse");
    REQUIRE(PollStatus() == "Connecting...");
}

TEST_CASE("an open network is submitted with an empty password", "[T-6][FR-14]")
{
    StartPortal();
    g_scan_entries = {Entry("cafe", -50, true)};
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    Submit("ssid=cafe&password=");

    REQUIRE(Status() == "200 OK");
    REQUIRE(g_submissions.size() == 1);
    REQUIRE(std::string(g_submissions[0].password).empty());
}

TEST_CASE("invalid submissions are rejected without a connection attempt", "[T-6][FR-13][FR-14]")
{
    StartPortal();
    g_scan_entries = {Entry("home", -50), Entry("cafe", -60, true), Entry("corp", -70, false, true)};
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    const std::string long_ssid(33, 's');
    const char *const forms[] = {
        "ssid=&password=correct+horse",                                   // empty SSID
        "ssid=home&password=short",                                       // password too short
        "ssid=home&password=",                                            // secured network without password
        "ssid=cafe&password=surprise-pw",                                 // open network with a password
        "ssid=corp&password=correct+horse",                               // enterprise network
        "password=correct+horse",                                         // no SSID field
        "ssid=home",                                                      // no password field
        "ssid=home&password=bad%zzescape",                                // broken encoding
        "ssid=a%00b&password=correct+horse",                              // embedded NUL
        "",                                                               // empty body
    };
    for (const char *form : forms) {
        INFO(form);
        Submit(form);
        REQUIRE(Status() == "400 Bad Request");
    }
    Submit(("ssid=" + long_ssid + "&password=correct+horse").c_str());    // 33-byte SSID
    REQUIRE(Status() == "400 Bad Request");
    Submit(("ssid=home&password=" + std::string(65, 'p')).c_str());       // 65-character password
    REQUIRE(Status() == "400 Bad Request");

    REQUIRE(g_submissions.empty());
    REQUIRE(PollStatus().empty());                                        // no trial: status untouched
}

TEST_CASE("an oversized form is refused before parsing", "[T-6][FR-14]")
{
    StartPortal();
    Submit("ssid=home&password=" + std::string(400, 'a'));
    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(g_submissions.empty());
}

TEST_CASE("the largest legal encoded form is accepted", "[T-6][FR-14]")
{
    StartPortal();
    // 32-byte SSID and 63-character password where every byte is percent-encoded.
    std::string form = "ssid=";
    for (int index = 0; index < 32; ++index) form += "%41";
    form += "&password=";
    for (int index = 0; index < 63; ++index) form += "%42";
    REQUIRE(form.size() <= HTTP_PORTAL_FORM_MAX);

    Submit(form);

    REQUIRE(Status() == "200 OK");
    REQUIRE(g_submissions.size() == 1);
    REQUIRE(std::string(g_submissions[0].ssid) == std::string(32, 'A'));
    REQUIRE(std::string(g_submissions[0].password) == std::string(63, 'B'));
}

TEST_CASE("a submission while a trial is running is refused and leaves the status alone", "[T-6][FR-22]")
{
    StartPortal();
    Submit("ssid=home&password=correct+horse");
    REQUIRE(PollStatus() == "Connecting...");
    SetHttpPortalStatus(PORTAL_STATUS_CONNECTED);

    g_submit_accepts = false;                        // the orchestrator is busy
    Submit("ssid=other&password=correct+horse");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(Body().find("Busy") != std::string::npos);
    REQUIRE(PollStatus() == "Connected successfully");
}

// ---- Status endpoint ------------------------------------------------------------------------------------------------

TEST_CASE("the status endpoint returns the three plain-text results", "[T-6][FR-22]")
{
    StartPortal();

    REQUIRE(PollStatus().empty());                                        // nothing submitted yet
    REQUIRE(std::string(TestHttpdContentType()) == "text/plain");

    SetHttpPortalStatus(PORTAL_STATUS_CONNECTING);
    REQUIRE(PollStatus() == "Connecting...");
    SetHttpPortalStatus(PORTAL_STATUS_CONNECTED);
    REQUIRE(PollStatus() == "Connected successfully");
    SetHttpPortalStatus(PORTAL_STATUS_FAILED);
    REQUIRE(PollStatus() == "Connection failed");
    REQUIRE(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
}

TEST_CASE("starting the portal clears an old result", "[T-6][FR-22]")
{
    StartPortal();
    SetHttpPortalStatus(PORTAL_STATUS_FAILED);
    REQUIRE(StartHttpPortal(&kOps));
    REQUIRE(PollStatus().empty());
}

// ---- Password handling ----------------------------------------------------------------------------------------------

TEST_CASE("the password never appears in a response, header or log", "[T-7][FR-19][NFR-2]")
{
    StartPortal();
    g_scan_entries = {Entry("home", -50)};
    TestHttpdRequest(HTTP_GET, "/", nullptr);
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);

    Submit("ssid=home&password=Sup3rSecretPw%21");                        // accepted
    PollStatus();
    Submit("ssid=home&password=Sup3rSecretPw%21&junk");                   // still parses; make a rejected one too
    Submit("ssid=corp-unknown&password=Sup3rSecretPw%21%00");             // rejected
    SetHttpPortalStatus(PORTAL_STATUS_FAILED);
    PollStatus();
    TestHttpdRequest(HTTP_GET, "/generate_204?password=none", nullptr);

    REQUIRE(std::string(TestHttpdAllOutput()).find("Sup3rSecretPw") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("Sup3rSecretPw") == std::string::npos);
}

TEST_CASE("the submitted SSID is logged only at Debug", "[T-7][FR-19]")
{
    StartPortal();
    g_scan_entries = {Entry("NeighbourNet", -50)};
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);
    Submit("ssid=NeighbourNet&password=correct+horse");

    const std::string log = TestLogText();
    REQUIRE(log.find("NeighbourNet") != std::string::npos);
    for (size_t at = log.find("NeighbourNet"); at != std::string::npos; at = log.find("NeighbourNet", at + 1)) {
        const size_t line_start = log.rfind('\n', at);
        REQUIRE(log.substr(line_start == std::string::npos ? 0 : line_start + 1, 4) == "[L0 ");
    }
}

// ---- Slow uploads and locking ---------------------------------------------------------------------------------------

TEST_CASE("a slow form upload is retried after receive timeouts", "[T-6][FR-14]")
{
    StartPortal();
    TestHttpdRecvTimeouts(HTTP_PORTAL_RECV_RETRIES);            // every allowed retry is used
    Submit("ssid=home&password=correct+horse");

    REQUIRE(Status() == "200 OK");
    REQUIRE(g_submissions.size() == 1);
}

TEST_CASE("an upload that keeps timing out is refused", "[T-6][FR-14]")
{
    StartPortal();
    TestHttpdRecvTimeouts(HTTP_PORTAL_RECV_RETRIES + 1);
    Submit("ssid=home&password=correct+horse");

    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(g_submissions.empty());
}

TEST_CASE("the status lock is always released", "[T-8][NFR-9]")
{
    StartPortal();
    SetHttpPortalStatus(PORTAL_STATUS_CONNECTING);
    PollStatus();
    Submit("ssid=home&password=correct+horse");
    PollStatus();
    StartHttpPortal(&kOps);
    REQUIRE(MockGetMutexBalance() == 0);
}

TEST_CASE("unsupported networks are marked in the scan and cannot be submitted", "[T-5][T-6][FR-13]")
{
    StartPortal();
    g_scan_entries = {Entry("legacy", -50, false, true)};       // e.g. WEP or WPA1-only
    TestHttpdRequest(HTTP_GET, "/scan", nullptr);
    REQUIRE(Body().find("\"uns\":true") != std::string::npos);

    Submit("ssid=legacy&password=correct+horse");
    REQUIRE(Status() == "400 Bad Request");
    REQUIRE(g_submissions.empty());
}
