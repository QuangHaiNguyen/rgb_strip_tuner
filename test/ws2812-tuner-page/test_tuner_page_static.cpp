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

TEST_CASE("exactly two range sliders and exactly two fetch() calls", "[SPEC-003][T-7][FR-7][FR-11][NFR-17]")
{
    // Changed 2026-10-03: the bounded result poll adds the second fetch() (FR-27); was exactly one.
    const std::string page = Page();
    REQUIRE(CountOf(page, "type=range") == 2);
    REQUIRE(CountOf(page, "type=\"range\"") == 0);   // no second, quoted spelling slipping past the count
    REQUIRE(CountOf(page, "fetch(") == 2);
    REQUIRE(page.find("fetch('/tuner',{method:'POST'") != std::string::npos);
    REQUIRE(page.find("fetch('/tuner/result?seq='+n,{cache:'no-store'})") != std::string::npos);
}

TEST_CASE("exactly one setTimeout( with the literal 250 and no setInterval", "[SPEC-003][T-7][FR-11][FR-27][NFR-20]")
{
    // Changed 2026-10-03: FR-11 allows exactly one setTimeout chain for the poll; was no timer at all.
    const std::string page = Page();
    REQUIRE(CountOf(page, "setTimeout(") == 1);
    REQUIRE(CountOf(page, "setTimeout") == 1);
    REQUIRE(CountOf(page, "setInterval") == 0);
    const size_t timer_at = page.find("setTimeout(()=>{");
    REQUIRE(timer_at != std::string::npos);
    REQUIRE(CountOf(page, "},250)") == 1);                  // the timer's delay argument
    REQUIRE(page.find("},250)") > timer_at);
    REQUIRE(CountOf(page, ",250)") == 1);                   // one 250 ms delay, no second timer argument
    REQUIRE(page.find("i<8") != std::string::npos);   // TUNER_RESULT_POLL_MAX
}

TEST_CASE("the literal V4 expression, the V4 text and the pre-line status", "[SPEC-003][T-7][FR-10][FR-28]")
{
    const std::string page = Page();
    REQUIRE(CountOf(page, "+b0h.value*+b1p.value>=+b1h.value*+b0p.value") == 1);
    REQUIRE(page.find("'Bit 0 duty must be less than bit 1 duty'") != std::string::npos);
    REQUIRE(page.find("#st{white-space:pre-line}") != std::string::npos);
    REQUIRE(page.find("r.headers.get('Tuner-Seq')") != std::string::npos);
    for (const char *text : {"Measurement failed: no signal", "Measurement failed: bad capture", "Not measured",
                             "Superseded by a newer send", "Measurement not available", " ns measured",
                             "GRB match ", "Send failed, check connection", "Invalid values", "Sending..."}) {
        INFO(text);
        REQUIRE(page.find(text) != std::string::npos);
    }
    REQUIRE(page.find("innerHTML") == std::string::npos);
}

TEST_CASE("no forbidden construct is present", "[SPEC-003][T-7][NFR-1][NFR-17]")
{
    const std::string page = Page();
    const char *const kForbidden[] = {
        "setInterval", "innerHTML", "<svg", "<canvas", "<img", "<!--", "<noscript",
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

TEST_CASE("the only URL references are /, /tuner, /tuner/result?seq= and the data: favicon", "[SPEC-003][T-7][NFR-1]")
{
    const std::string page = Page();

    const std::vector<std::string> hrefs = AllMatches(page, R"(href=("[^"]*"|[^ >]*))");
    REQUIRE(hrefs == std::vector<std::string>{"\"data:,\"", "/"});

    // Changed 2026-10-03: the poll URL /tuner/result?seq= is the third relative URL (NFR-1).
    REQUIRE(AllMatches(page, R"(fetch\('([^']*)')") == std::vector<std::string>{"/tuner/result?seq=", "/tuner"});
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
