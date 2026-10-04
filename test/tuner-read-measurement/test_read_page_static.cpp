/**
 * @file test_read_page_static.cpp
 * @brief SPEC-006 T-6, static part (FR-1, FR-4, FR-5, NFR-1, NFR-10): checks of g_tuner_page and g_tuner_page_station.
 *
 * Links main/http_portal/tuner_page.c alone. The served body is the string sent with HTTPD_RESP_USE_STRLEN, so
 * strlen() is the Content-Length.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

extern "C" {
#include "tuner_page.h"
#include "ws2812_timing.h"
}

namespace {

std::string Provisioning() { return std::string(g_tuner_page); }
std::string Station() { return std::string(g_tuner_page_station); }

size_t CountOf(const std::string &text, const std::string &token)
{
    size_t count = 0;
    for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + 1)) {
        ++count;
    }
    return count;
}

std::vector<std::string> AllMatches(const std::string &text, const std::string &pattern)
{
    std::vector<std::string> found;
    const std::regex expression(pattern);
    for (std::sregex_iterator it(text.begin(), text.end(), expression), end; it != end; ++it) {
        found.push_back((*it)[1].str());
    }
    return found;
}

}  // namespace

TEST_CASE("both pages are at most 5,120 bytes", "[T-6][NFR-1]")
{
    const size_t provisioning = std::strlen(g_tuner_page);
    const size_t station = std::strlen(g_tuner_page_station);
    CAPTURE(provisioning, station);
    REQUIRE(TUNER_PAGE_MAX_BYTES == 5120);
    REQUIRE(provisioning <= 5120);
    REQUIRE(station <= 5120);
    REQUIRE(provisioning > 4096);   // informational bound: the Read additions really grew the page past the old cap
}

TEST_CASE("the pages differ by exactly the 18-byte Back link", "[T-6][NFR-1][SPEC-005]")
{
    const std::string provisioning = Provisioning();
    const std::string station = Station();
    REQUIRE(provisioning.size() - station.size() == 18);
    const std::string back = "<a href=/>Back</a>";
    REQUIRE(back.size() == 18);
    const size_t at = provisioning.find(back);
    REQUIRE(at != std::string::npos);
    REQUIRE(provisioning.substr(0, at) + provisioning.substr(at + back.size()) == station);
}

TEST_CASE("each page has exactly one Read button, type=button, between Send and Defaults", "[T-6][FR-1]")
{
    for (const std::string &page : {Provisioning(), Station()}) {
        const std::vector<std::string> buttons = AllMatches(page, R"(<button[^>]*>([^<]*)</button>)");
        REQUIRE(buttons == std::vector<std::string>{"Send", "Read", "Defaults"});
        REQUIRE(CountOf(page, ">Read</button>") == 1);
        REQUIRE(page.find("<button type=button id=rd>Read</button>") != std::string::npos);
        // Same CSS as the other buttons: no inline style or class of its own.
        const size_t read_at = page.find("id=rd");
        const std::string tag = page.substr(page.rfind('<', read_at), page.find('>', read_at) - page.rfind('<', read_at));
        REQUIRE(tag.find("style") == std::string::npos);
        REQUIRE(tag.find("class") == std::string::npos);
    }
}

TEST_CASE("/tuner/read is the only new relative URL; it is POSTed without a body", "[T-6][FR-2][FR-5][NFR-1]")
{
    for (const std::string &page : {Provisioning(), Station()}) {
        REQUIRE(AllMatches(page, R"(fetch\('([^']*)')") ==
                std::vector<std::string>{"/tuner/result?seq=", "/tuner", "/tuner/read"});
        REQUIRE(page.find("fetch('/tuner/read',{method:'POST'})") != std::string::npos);
        REQUIRE(CountOf(page, "/tuner/read") == 1);
        REQUIRE(page.find("src=") == std::string::npos);
        REQUIRE(page.find("http://") == std::string::npos);
        REQUIRE(page.find("https://") == std::string::npos);
    }
}

TEST_CASE("still one setTimeout( chain, no setInterval, no innerHTML", "[T-6][FR-4][FR-5]")
{
    for (const std::string &page : {Provisioning(), Station()}) {
        REQUIRE(CountOf(page, "setTimeout(") == 1);
        REQUIRE(CountOf(page, "},250)") == 1);
        REQUIRE(CountOf(page, "setInterval") == 0);
        REQUIRE(CountOf(page, "innerHTML") == 0);
        REQUIRE(CountOf(page, "outerHTML") == 0);
        REQUIRE(CountOf(page, "insertAdjacentHTML") == 0);
        REQUIRE(CountOf(page, "fetch(") == 3);
        REQUIRE(page.find("white-space:pre-line") != std::string::npos);
    }
}

TEST_CASE("the FR-4 / FR-3 page texts are present; no duty text is added for Read", "[T-6][FR-3][FR-4]")
{
    for (const std::string &page : {Provisioning(), Station()}) {
        for (const char *text : {"Reading...", "Read failed, check connection", "not found", "high ", " ns, period ",
                                 "Measurement failed: no signal", "Measurement failed: bad capture", "Not measured",
                                 "Superseded by a newer send", "Measurement not available"}) {
            INFO(text);
            REQUIRE(page.find(text) != std::string::npos);
        }
        REQUIRE(page.find("'read'") != std::string::npos);   // the new final state is recognized
    }
}
