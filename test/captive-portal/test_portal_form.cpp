/**
 * @file test_portal_form.cpp
 * @brief Host tests for the portal's pure helpers (SPEC-002 T-5, T-6; FR-11, FR-13, FR-14, FR-19).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "http_portal.h"
}

namespace {

std::string Decode(const std::string &input, bool *ok = nullptr, size_t output_size = 64)
{
    std::string output(output_size, '\0');
    bool result = DecodeFormValue(input.data(), input.size(), &output[0], output.size());
    if (ok != nullptr) {
        *ok = result;
    }
    return result ? std::string(output.c_str()) : std::string();
}

std::string Field(const std::string &body, const std::string &name, bool *ok = nullptr)
{
    char value[64] = {};
    bool result = GetFormField(body.c_str(), name.c_str(), value, sizeof(value));
    if (ok != nullptr) {
        *ok = result;
    }
    return std::string(value);
}

wifi_scan_entry_t Entry(const char *ssid, bool open, bool unsupported)
{
    wifi_scan_entry_t entry = {};
    std::strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    entry.is_open = open;
    entry.is_unsupported = unsupported;
    return entry;
}

wifi_credentials_t Credentials(const char *ssid, const char *password)
{
    wifi_credentials_t credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

}  // namespace

TEST_CASE("form values are percent-decoded", "[T-6][FR-14]")
{
    bool ok = false;
    REQUIRE(Decode("plain", &ok) == "plain");
    REQUIRE(ok);
    REQUIRE(Decode("a+b+c") == "a b c");
    REQUIRE(Decode("100%25") == "100%");
    REQUIRE(Decode("%41%62%63") == "Abc");
    REQUIRE(Decode("%e2%82%ac") == "\xE2\x82\xAC");      // UTF-8 bytes survive
    REQUIRE(Decode("p%40ss%3Dw%26rd") == "p@ss=w&rd");
    REQUIRE(Decode("", &ok) == "");
    REQUIRE(ok);
}

TEST_CASE("bad escapes and oversized values are rejected", "[T-6][FR-14]")
{
    bool ok = true;
    Decode("%zz", &ok);
    REQUIRE_FALSE(ok);
    Decode("%4", &ok);
    REQUIRE_FALSE(ok);
    Decode("abc%", &ok);
    REQUIRE_FALSE(ok);
    Decode("a%00b", &ok);       // an embedded NUL would truncate the SSID/password
    REQUIRE_FALSE(ok);
    Decode("12345678", &ok, 8); // needs 9 bytes with the terminator
    REQUIRE_FALSE(ok);
    Decode("1234567", &ok, 8);
    REQUIRE(ok);
    Decode("x", &ok, 0);
    REQUIRE_FALSE(ok);
}

TEST_CASE("fields are found by exact name", "[T-6][FR-14]")
{
    bool ok = false;
    REQUIRE(Field("ssid=home&password=secret123", "ssid", &ok) == "home");
    REQUIRE(ok);
    REQUIRE(Field("ssid=home&password=secret123", "password") == "secret123");
    REQUIRE(Field("a=1&ssid=mid&b=2", "ssid") == "mid");
    REQUIRE(Field("ssid=x+y%26z&password=", "ssid") == "x y&z");

    Field("ssid=home&password=secret123", "pass", &ok);        // prefix of a name is not a match
    REQUIRE_FALSE(ok);
    Field("xssid=home", "ssid", &ok);                          // suffix of a name is not a match
    REQUIRE_FALSE(ok);
    Field("ssid=home", "password", &ok);
    REQUIRE_FALSE(ok);
    Field("password=ssid%3Dtrick", "ssid", &ok);               // "ssid=" inside a value is not a field
    REQUIRE_FALSE(ok);
    Field("", "ssid", &ok);
    REQUIRE_FALSE(ok);
    Field("ssid=home", "", &ok);
    REQUIRE_FALSE(ok);
}

TEST_CASE("an empty value is a valid field", "[T-6][FR-14]")
{
    bool ok = false;
    REQUIRE(Field("ssid=open-net&password=", "password", &ok) == "");
    REQUIRE(ok);
}

TEST_CASE("JSON escaping neutralizes quotes, control characters and markup", "[T-5][FR-11]")
{
    char out[256];

    REQUIRE(EscapeJsonString("plain SSID", out, sizeof(out)) == 10);
    REQUIRE(std::string(out) == "plain SSID");

    EscapeJsonString("say \"hi\"\\", out, sizeof(out));
    REQUIRE(std::string(out) == "say \\\"hi\\\"\\\\");

    EscapeJsonString("a\nb\tc\x01", out, sizeof(out));
    REQUIRE(std::string(out) == "a\\u000ab\\u0009c\\u0001");

    EscapeJsonString("<script>alert('x')&</script>", out, sizeof(out));
    const std::string escaped = out;
    REQUIRE(escaped.find('<') == std::string::npos);
    REQUIRE(escaped.find('>') == std::string::npos);
    REQUIRE(escaped.find('&') == std::string::npos);
    REQUIRE(escaped.find('\'') == std::string::npos);
    REQUIRE(escaped.find("\\u003cscript\\u003e") == 0);

    REQUIRE(std::string(out).find('"') == std::string::npos);
    EscapeJsonString("caf\xC3\xA9", out, sizeof(out));         // UTF-8 passes through
    REQUIRE(std::string(out) == "caf\xC3\xA9");
}

TEST_CASE("JSON escaping never overruns the output buffer", "[T-5][FR-11]")
{
    char out[8];
    REQUIRE(EscapeJsonString("1234567", out, sizeof(out)) == 7);
    REQUIRE(EscapeJsonString("12345678", out, sizeof(out)) == 0);
    REQUIRE(EscapeJsonString("<", out, sizeof(out)) == 6);       // 6 chars + terminator fit exactly
    REQUIRE(EscapeJsonString("<<", out, sizeof(out)) == 0);
    REQUIRE(EscapeJsonString("x", out, 0) == 0);
}

TEST_CASE("submission validation follows the scan result", "[T-6][FR-13][FR-14]")
{
    const wifi_scan_entry_t list[] = {Entry("cafe", true, false), Entry("home", false, false), Entry("corp", false, true)};
    auto valid = [&](const char *ssid, const char *password) {
        const wifi_credentials_t credentials = Credentials(ssid, password);
        return ValidateSubmission(&credentials, list, 3);
    };

    SECTION("open network needs an empty password") {
        REQUIRE(valid("cafe", ""));
        REQUIRE_FALSE(valid("cafe", "unneeded-pw"));
    }
    SECTION("secured network needs a valid password") {
        REQUIRE(valid("home", "correct horse"));
        REQUIRE_FALSE(valid("home", ""));
        REQUIRE_FALSE(valid("home", "short"));
    }
    SECTION("enterprise networks are refused") {
        REQUIRE_FALSE(valid("corp", "whatever-pw"));
        REQUIRE_FALSE(valid("corp", ""));
    }
    SECTION("a network missing from the last scan needs a password") {
        REQUIRE(valid("gone", "correct horse"));
        REQUIRE_FALSE(valid("gone", ""));
    }
    SECTION("malformed credentials") {
        REQUIRE_FALSE(valid("", "correct horse"));
        REQUIRE_FALSE(ValidateSubmission(nullptr, list, 3));
    }
    SECTION("no scan yet") {
        const wifi_credentials_t credentials = Credentials("home", "correct horse");
        REQUIRE(ValidateSubmission(&credentials, nullptr, 0));
    }
}
