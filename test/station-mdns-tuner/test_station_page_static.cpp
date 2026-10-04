/**
 * @file test_station_page_static.cpp
 * @brief SPEC-005 T-5 (FR-15, NFR-15): static checks of the station page g_tuner_page_station, in the style of
 *        test/ws2812-tuner-page/test_tuner_page_static.cpp (SPEC-003 T-7).
 *
 * Links main/http_portal/tuner_page.c alone. The station page is sent with HTTPD_RESP_USE_STRLEN, so strlen() is the
 * Content-Length. `.rodata` placement is a map-file check (T-7/T-17) and is not host-testable.
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

const std::string kBackLink = "<a href=/>Back</a>";

std::string Station() { return std::string(g_tuner_page_station); }
std::string Provisioning() { return std::string(g_tuner_page); }

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

TEST_CASE("the station page is the tuner page with exactly the Back link removed (byte comparison)", "[T-5][FR-15]")
{
    const std::string provisioning = Provisioning();
    REQUIRE(kBackLink.size() == 18);
    REQUIRE(CountOf(provisioning, kBackLink) == 1);

    std::string expected = provisioning;
    expected.erase(provisioning.find(kBackLink), kBackLink.size());
    REQUIRE(Station() == expected);
}

TEST_CASE("the station page is exactly 18 bytes shorter than the tuner page", "[T-5][FR-15]")
{
    REQUIRE(std::strlen(g_tuner_page_station) == std::strlen(g_tuner_page) - 18);
}

TEST_CASE("the station page contains neither Back nor href=/>", "[T-5][FR-15]")
{
    const std::string page = Station();
    REQUIRE(page.find("Back") == std::string::npos);
    REQUIRE(page.find("href=/>") == std::string::npos);
    REQUIRE(page.find("<a ") == std::string::npos);                // no other link either
}

TEST_CASE("the station page is at most 5,120 bytes", "[T-5][FR-15]")
{
    const size_t length = std::strlen(g_tuner_page_station);
    CAPTURE(length);
    REQUIRE(length > 0);
    REQUIRE(length <= TUNER_PAGE_MAX_BYTES);
    REQUIRE(TUNER_PAGE_MAX_BYTES == 5120);   // SPEC-006 NFR-1 (2026-10-04): was 4,096
}

TEST_CASE("the provisioning tuner page is unchanged: the Back link is still there once", "[T-5][FR-15][FR-30]")
{
    const std::string page = Provisioning();
    REQUIRE(CountOf(page, kBackLink) == 1);
    REQUIRE(page.find("<h1>WS2812 timing tuner</h1><a href=/>Back</a><form id=f>") != std::string::npos);
    REQUIRE(std::strlen(g_tuner_page) <= TUNER_PAGE_MAX_BYTES);
}

TEST_CASE("the two pages are distinct constants", "[T-5][FR-15]")
{
    REQUIRE(static_cast<const void *>(g_tuner_page_station) != static_cast<const void *>(g_tuner_page));
}

// ---- SPEC-003 T-7 static checks applied to the station page (FR-15: "its only relative URL is /tuner") ----------

TEST_CASE("station page: the only URL references are /tuner, /tuner/result?seq= and the data: favicon", "[T-5][FR-15][NFR-15]")
{
    // Changed 2026-10-03: the result poll adds /tuner/result?seq= (FR-15: relative URLs /tuner and /tuner/result).
    const std::string page = Station();
    REQUIRE(AllMatches(page, R"(href=("[^"]*"|[^ >]*))") == std::vector<std::string>{"\"data:,\""});
    // SPEC-006 NFR-1 (2026-10-04): /tuner/read is the only new relative URL.
    REQUIRE(AllMatches(page, R"(fetch\('([^']*)')") ==
            std::vector<std::string>{"/tuner/result?seq=", "/tuner", "/tuner/read"});
    REQUIRE(page.find("src=") == std::string::npos);
    REQUIRE(page.find("action=") == std::string::npos);
    REQUIRE(page.find("url(") == std::string::npos);
}

TEST_CASE("station page: no external reference and no forbidden construct", "[T-5][FR-15][NFR-15]")
{
    const std::string page = Station();
    const char *const kForbidden[] = {
        // setTimeout is allowed once since 2026-10-03 (checked below)
        "setInterval", "innerHTML", "<svg", "<canvas", "<img", "<!--", "<noscript",
        "/*", "@font-face", "http:", "https:", "//",
    };
    for (const char *token : kForbidden) {
        INFO("token: " << token);
        REQUIRE(page.find(token) == std::string::npos);
    }
}

TEST_CASE("station page: same form, sliders, script, two fetch() and one setTimeout( as the tuner page", "[T-5][FR-15][NFR-15]")
{
    // Changed 2026-10-03: the shared script now has the POST and the poll fetch() and one 250 ms setTimeout chain.
    const std::string page = Station();
    REQUIRE(CountOf(page, "type=range") == 2);
    REQUIRE(CountOf(page, "fetch(") == 3);   // SPEC-006 FR-5 (2026-10-04): + POST /tuner/read; was 2
    REQUIRE(CountOf(page, "setTimeout(") == 1);
    REQUIRE(CountOf(page, "},250)") == 1);
    REQUIRE(CountOf(page, "setInterval") == 0);
    REQUIRE(CountOf(page, "+b0h.value*+b1p.value>=+b1h.value*+b0p.value") == 1);
    REQUIRE(page.find("#st{white-space:pre-line}") != std::string::npos);
    REQUIRE(AllMatches(page, R"(<input id=([A-Za-z0-9_]+))") ==
            std::vector<std::string>{"b0h", "b0p", "b1h", "b1p", "rst"});
    REQUIRE(page.find("<button type=button id=sd>Send</button>") != std::string::npos);
    REQUIRE(page.find("<meta name=viewport content=\"width=device-width,initial-scale=1\">") != std::string::npos);
    REQUIRE(page.rfind("</script></body></html>") == page.size() - std::strlen("</script></body></html>"));
}

TEST_CASE("station page: minified, no indentation, tabs or blank lines", "[T-5][FR-15]")
{
    const std::string page = Station();
    REQUIRE(page.find('\t') == std::string::npos);
    REQUIRE(page.find("\n\n") == std::string::npos);
    REQUIRE(page.find("\n ") == std::string::npos);
    REQUIRE(page.find("  ") == std::string::npos);
}
