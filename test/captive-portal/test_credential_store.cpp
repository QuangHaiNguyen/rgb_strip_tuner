/**
 * @file test_credential_store.cpp
 * @brief Host tests for the encrypted credential store (SPEC-002 T-2, T-7, T-12; FR-14, FR-17, NFR-1, NFR-3, NFR-8).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

extern "C" {
#include "credential_store.h"
#include "host_stubs.h"
#include "nvs_fake.h"
}

namespace {

wifi_credentials_t MakeCredentials(const char *ssid, const char *password)
{
    wifi_credentials_t credentials = {};
    std::strncpy(credentials.ssid, ssid, sizeof(credentials.ssid) - 1);
    std::strncpy(credentials.password, password, sizeof(credentials.password) - 1);
    return credentials;
}

bool Equal(const wifi_credentials_t &a, const wifi_credentials_t &b)
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

void SeedRecord(const char *key, const wifi_credentials_t &credentials)
{
    TestNvsSeed(key, &credentials, sizeof(credentials));
}

void Reset()
{
    TestNvsReset();
    TestLogReset();
}

const wifi_credentials_t kOld = MakeCredentials("old-net", "old-password");
const wifi_credentials_t kNew = MakeCredentials("new-net", "new-password");

}  // namespace

TEST_CASE("SSID must be 1..32 bytes", "[T-6][FR-14]")
{
    wifi_credentials_t credentials = MakeCredentials("a", "");
    REQUIRE(IsCredentialsValid(&credentials));

    credentials = MakeCredentials(std::string(32, 'x').c_str(), "");
    REQUIRE(IsCredentialsValid(&credentials));

    credentials = MakeCredentials("", "");
    REQUIRE_FALSE(IsCredentialsValid(&credentials));

    std::memset(&credentials, 'x', sizeof(credentials.ssid));  // 33 bytes, no terminator
    REQUIRE_FALSE(IsCredentialsValid(&credentials));
    REQUIRE_FALSE(IsCredentialsValid(nullptr));
}

TEST_CASE("password rules: empty, 8-63 printable ASCII, or 64 hex", "[T-6][FR-14]")
{
    auto valid = [](const std::string &password) {
        wifi_credentials_t credentials = MakeCredentials("net", password.c_str());
        return IsCredentialsValid(&credentials);
    };

    REQUIRE(valid(""));
    REQUIRE_FALSE(valid("1234567"));
    REQUIRE(valid("12345678"));
    REQUIRE(valid("pass word with spaces & symbols !~"));
    REQUIRE(valid(std::string(63, 'p')));
    REQUIRE(valid(std::string(64, 'a')));                       // 64 hex digits
    REQUIRE(valid(std::string(32, 'A') + std::string(32, '9')));
    REQUIRE_FALSE(valid(std::string(63, 'g') + "f"));          // 64 chars, not all hex
    REQUIRE_FALSE(valid("pass\tword1"));                        // control character
    REQUIRE_FALSE(valid("pässword12"));                         // non-ASCII

    wifi_credentials_t unterminated = MakeCredentials("net", "");
    std::memset(unterminated.password, 'a', sizeof(unterminated.password));  // 65 bytes
    REQUIRE_FALSE(IsCredentialsValid(&unterminated));
}

TEST_CASE("secure NVS failure fails closed with an Error log", "[T-2][T-12][NFR-8]")
{
    Reset();
    SeedRecord("cred_active", kOld);
    TestNvsInitResult(ESP_FAIL);

    REQUIRE_FALSE(InitCredentialStore());
    REQUIRE(TestLogCount(3) >= 1);

    wifi_credentials_t loaded = {};
    REQUIRE_FALSE(LoadCredentials(&loaded));
    REQUIRE_FALSE(ReplaceCredentials(&kNew));
    REQUIRE(TestNvsOpCount() == 0);
    REQUIRE(loaded.ssid[0] == '\0');
}

TEST_CASE("no free NVS pages also fails closed and never erases", "[T-2][NFR-8]")
{
    Reset();
    TestNvsInitResult(ESP_ERR_NVS_NO_FREE_PAGES);
    REQUIRE_FALSE(InitCredentialStore());
    REQUIRE(TestNvsOpCount() == 0);
}

TEST_CASE("load returns only a complete valid active record", "[T-2][FR-4][FR-5]")
{
    Reset();
    wifi_credentials_t loaded = {};

    SECTION("no record") {
        REQUIRE(InitCredentialStore());
        REQUIRE_FALSE(LoadCredentials(&loaded));
    }
    SECTION("valid record") {
        SeedRecord("cred_active", kOld);
        REQUIRE(InitCredentialStore());
        REQUIRE(LoadCredentials(&loaded));
        REQUIRE(Equal(loaded, kOld));
    }
    SECTION("truncated record is treated as no credentials") {
        const char partial[10] = "short";
        TestNvsSeed("cred_active", partial, sizeof(partial));
        REQUIRE(InitCredentialStore());
        REQUIRE_FALSE(LoadCredentials(&loaded));
        REQUIRE(loaded.ssid[0] == '\0');
    }
    SECTION("record with an empty SSID is rejected") {
        SeedRecord("cred_active", MakeCredentials("", ""));
        REQUIRE(InitCredentialStore());
        REQUIRE_FALSE(LoadCredentials(&loaded));
    }
    SECTION("null output buffer") {
        REQUIRE(InitCredentialStore());
        REQUIRE_FALSE(LoadCredentials(nullptr));
    }
    SECTION("store not initialized") {
        TestNvsInitResult(ESP_FAIL);
        InitCredentialStore();
        SeedRecord("cred_active", kOld);
        REQUIRE_FALSE(LoadCredentials(&loaded));
    }
}

TEST_CASE("replace writes pending, then active, then erases pending", "[T-7][FR-17]")
{
    Reset();
    SeedRecord("cred_active", kOld);
    REQUIRE(InitCredentialStore());

    REQUIRE(ReplaceCredentials(&kNew));

    REQUIRE(std::string(TestNvsSetOrder()) == "cred_pending,cred_active");
    wifi_credentials_t stored = {};
    size_t length = 0;
    REQUIRE(TestNvsRead("cred_active", &stored, sizeof(stored), &length));
    REQUIRE(length == sizeof(stored));
    REQUIRE(Equal(stored, kNew));
    REQUIRE_FALSE(TestNvsHasKey("cred_pending"));

    wifi_credentials_t loaded = {};
    REQUIRE(LoadCredentials(&loaded));
    REQUIRE(Equal(loaded, kNew));
}

TEST_CASE("replace with no previous record creates the active record", "[T-7][FR-17]")
{
    Reset();
    REQUIRE(InitCredentialStore());
    REQUIRE(ReplaceCredentials(&kNew));
    wifi_credentials_t loaded = {};
    REQUIRE(LoadCredentials(&loaded));
    REQUIRE(Equal(loaded, kNew));
}

TEST_CASE("invalid credentials are rejected without touching NVS", "[T-7][FR-14][FR-16]")
{
    Reset();
    SeedRecord("cred_active", kOld);
    REQUIRE(InitCredentialStore());

    wifi_credentials_t short_password = MakeCredentials("net", "short");
    REQUIRE_FALSE(ReplaceCredentials(&short_password));
    REQUIRE_FALSE(ReplaceCredentials(nullptr));
    REQUIRE(TestNvsOpCount() == 0);

    wifi_credentials_t loaded = {};
    REQUIRE(LoadCredentials(&loaded));
    REQUIRE(Equal(loaded, kOld));
}

TEST_CASE("failed pending verification preserves the previous active record", "[T-7][FR-16][FR-17]")
{
    Reset();
    SeedRecord("cred_active", kOld);
    REQUIRE(InitCredentialStore());

    SECTION("read-back of the pending record differs") {
        TestNvsCorruptReadback("cred_pending");
        REQUIRE_FALSE(ReplaceCredentials(&kNew));
    }
    SECTION("pending write fails") {
        TestNvsDieAfterOps(0);
        REQUIRE_FALSE(ReplaceCredentials(&kNew));
    }
    SECTION("pending commit fails") {
        TestNvsDieAfterOps(1);
        REQUIRE_FALSE(ReplaceCredentials(&kNew));
    }

    wifi_credentials_t stored = {};
    size_t length = 0;
    REQUIRE(TestNvsRead("cred_active", &stored, sizeof(stored), &length));
    REQUIRE(Equal(stored, kOld));
}

TEST_CASE("power loss at any point leaves a complete old or new pair", "[T-7][NFR-3]")
{
    for (int operations = 0; operations <= 8; ++operations) {
        INFO("device dies after " << operations << " NVS operations");
        Reset();
        SeedRecord("cred_active", kOld);
        REQUIRE(InitCredentialStore());

        TestNvsDieAfterOps(operations);
        const bool replaced = ReplaceCredentials(&kNew);
        TestNvsPowerLoss();      // uncommitted writes are gone
        TestNvsClearFailure();   // "reboot"
        REQUIRE(InitCredentialStore());

        wifi_credentials_t loaded = {};
        REQUIRE(LoadCredentials(&loaded));
        REQUIRE((Equal(loaded, kOld) || Equal(loaded, kNew)));
        if (replaced) {
            REQUIRE(Equal(loaded, kNew));
        }
        REQUIRE_FALSE(TestNvsHasKey("cred_pending"));
    }
}

TEST_CASE("boot recovery completes an interrupted replacement", "[T-7][NFR-3][S7]")
{
    Reset();

    SECTION("complete pending record is promoted and erased") {
        SeedRecord("cred_active", kOld);
        SeedRecord("cred_pending", kNew);
        REQUIRE(InitCredentialStore());
        wifi_credentials_t loaded = {};
        REQUIRE(LoadCredentials(&loaded));
        REQUIRE(Equal(loaded, kNew));
        REQUIRE_FALSE(TestNvsHasKey("cred_pending"));
    }
    SECTION("incomplete pending record is erased and active is kept") {
        SeedRecord("cred_active", kOld);
        const char partial[7] = "broken";
        TestNvsSeed("cred_pending", partial, sizeof(partial));
        REQUIRE(InitCredentialStore());
        wifi_credentials_t loaded = {};
        REQUIRE(LoadCredentials(&loaded));
        REQUIRE(Equal(loaded, kOld));
        REQUIRE_FALSE(TestNvsHasKey("cred_pending"));
    }
    SECTION("pending record with invalid content is erased") {
        SeedRecord("cred_pending", MakeCredentials("", "x"));
        REQUIRE(InitCredentialStore());
        wifi_credentials_t loaded = {};
        REQUIRE_FALSE(LoadCredentials(&loaded));
        REQUIRE_FALSE(TestNvsHasKey("cred_pending"));
    }
    SECTION("nothing pending: no writes at all") {
        SeedRecord("cred_active", kOld);
        REQUIRE(InitCredentialStore());
        REQUIRE(TestNvsOpCount() == 0);
    }
}

TEST_CASE("credentials never appear in log output", "[T-7][FR-19][NFR-2]")
{
    Reset();
    const wifi_credentials_t secret = MakeCredentials("secret-net", "Sup3rSecretPw!");
    REQUIRE(InitCredentialStore());
    REQUIRE(ReplaceCredentials(&secret));
    wifi_credentials_t loaded = {};
    REQUIRE(LoadCredentials(&loaded));
    TestNvsCorruptReadback("cred_pending");
    ReplaceCredentials(&kNew);

    REQUIRE(std::string(TestLogText()).find("Sup3rSecretPw!") == std::string::npos);
    REQUIRE(std::string(TestLogText()).find("new-password") == std::string::npos);
}

TEST_CASE("stored records are fixed-size blobs of the credential struct", "[T-12][NFR-1]")
{
    Reset();
    REQUIRE(InitCredentialStore());
    REQUIRE(ReplaceCredentials(&kNew));
    uint8_t buffer[128];
    size_t length = 0;
    REQUIRE(TestNvsRead("cred_active", buffer, sizeof(buffer), &length));
    REQUIRE(length == sizeof(wifi_credentials_t));  // one blob: SSID and password change together
}
