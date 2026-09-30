/**
 * @file test_tuner_page_static.cpp
 * @brief SPEC-003 T-7 (host part): static checks of the embedded tuner page string g_tuner_page
 *        (NFR-1, NFR-2, NFR-12 static parts, NFR-17; FR-7/FR-8 markup facts).
 *
 * Links main/http_portal/tuner_page.c alone. The served body is g_tuner_page sent with
 * HTTPD_RESP_USE_STRLEN, so strlen() is the Content-Length. The map/size part of T-7
 * (.rodata placement, .data+.bss growth) and the reviewer's CSS/JS-to-requirement mapping are
 * not host-testable and are left to `idf.py size` and review.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "tuner_page.h"
#include "ws2812_timing.h"
}

namespace {

std::string Page() { return std::string(g_tuner_page); }

size_t CountOf(const std::string &text, const std::string &token)
{
    size_t count = 0;
    for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + 1)) {
        ++count;
    }
    return count;
}

/** All captures of group 1 of @p pattern in @p text. */
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

TEST_CASE("the served page is at most TUNER_PAGE_MAX_BYTES (4,096) bytes", "[SPEC-003][T-7][NFR-2]")
{
    const size_t length = std::strlen(g_tuner_page);
    CAPTURE(length);
    REQUIRE(TUNER_PAGE_MAX_BYTES == 4096);
    REQUIRE(length > 0);
    REQUIRE(length <= TUNER_PAGE_MAX_BYTES);
}

TEST_CASE("exactly two range sliders and exactly one fetch() call", "[SPEC-003][T-7][FR-7][NFR-17]")
{
    const std::string page = Page();
    REQUIRE(CountOf(page, "type=range") == 2);
    REQUIRE(CountOf(page, "type=\"range\"") == 0);   // no second, quoted spelling slipping past the count
    REQUIRE(CountOf(page, "fetch(") == 1);
}

TEST_CASE("no forbidden construct is present", "[SPEC-003][T-7][NFR-1][NFR-17]")
{
    const std::string page = Page();
    const char *const kForbidden[] = {
        "setTimeout", "setInterval", "innerHTML", "<svg", "<canvas", "<img", "<!--", "<noscript",
        "/*", "@font-face", "http:", "https:",
    };
    for (const char *token : kForbidden) {
        INFO("token: " << token);
        REQUIRE(page.find(token) == std::string::npos);
    }
}

TEST_CASE("no protocol-relative // reference", "[SPEC-003][T-7][NFR-1]")
{
    // Covers "//host" URLs as well as JS line comments; neither is allowed in the minified page.
    REQUIRE(Page().find("//") == std::string::npos);
}

TEST_CASE("the only URL references are the relative paths / and /tuner plus the data: favicon", "[SPEC-003][T-7][NFR-1]")
{
    const std::string page = Page();

    const std::vector<std::string> hrefs = AllMatches(page, R"(href=("[^"]*"|[^ >]*))");
    REQUIRE(hrefs == std::vector<std::string>{"\"data:,\"", "/"});

    REQUIRE(AllMatches(page, R"(fetch\('([^']*)')") == std::vector<std::string>{"/tuner"});
    REQUIRE(page.find("src=") == std::string::npos);
    REQUIRE(page.find("action=") == std::string::npos);
    REQUIRE(page.find("url(") == std::string::npos);
}

TEST_CASE("viewport meta, lang, data: favicon and the Back link are present", "[SPEC-003][T-7][NFR-1][NFR-12]")
{
    const std::string page = Page();
    REQUIRE(page.find("<meta name=viewport content=\"width=device-width,initial-scale=1\">") != std::string::npos);
    REQUIRE(page.find("<html lang=") != std::string::npos);
    REQUIRE(page.find("<link rel=icon href=\"data:,\">") != std::string::npos);
    REQUIRE(page.find("href=/>") != std::string::npos);
}

TEST_CASE("exactly five inputs, each with a matching <label for=>", "[SPEC-003][T-7][FR-7][NFR-12]")
{
    const std::string page = Page();
    const std::vector<std::string> input_ids = AllMatches(page, R"(<input id=([A-Za-z0-9_]+))");
    REQUIRE(CountOf(page, "<input") == input_ids.size());   // every input carries an id
    REQUIRE(input_ids == std::vector<std::string>{"b0h", "b0p", "b1h", "b1p", "rst"});

    const std::vector<std::string> label_targets = AllMatches(page, R"(<label for=([A-Za-z0-9_]+))");
    REQUIRE(std::set<std::string>(label_targets.begin(), label_targets.end()) ==
            std::set<std::string>(input_ids.begin(), input_ids.end()));
    REQUIRE(label_targets.size() == input_ids.size());
}

TEST_CASE("the two sliders are the high-time inputs with the SPEC-003 range", "[SPEC-003][T-7][FR-7]")
{
    const std::string page = Page();
    REQUIRE(page.find("<input id=b0h type=range min=100 max=1200 step=25 value=400>") != std::string::npos);
    REQUIRE(page.find("<input id=b1h type=range min=100 max=1200 step=25 value=800>") != std::string::npos);
}

TEST_CASE("the pulse drawings are aria-hidden CSS boxes", "[SPEC-003][T-7][FR-8]")
{
    REQUIRE(CountOf(Page(), "<div class=w aria-hidden=true>") == 2);
}

TEST_CASE("minified: no indentation, tabs or blank lines", "[SPEC-003][T-7][NFR-17]")
{
    const std::string page = Page();
    REQUIRE(page.find('\t') == std::string::npos);
    REQUIRE(page.find("\n\n") == std::string::npos);
    REQUIRE(page.find("\n ") == std::string::npos);
    REQUIRE(page.find("  ") == std::string::npos);
}
