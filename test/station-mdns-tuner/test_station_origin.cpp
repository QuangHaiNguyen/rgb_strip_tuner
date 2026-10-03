/**
 * @file test_station_origin.cpp
 * @brief SPEC-005 T-19 (FR-27, NFR-13): IsHttpStationOriginAllowed() against the 22 reference vectors of section 7.5,
 *        one test case each, plus boundary cases around them.
 *
 * Links main/http_portal/station_origin.c alone: the predicate is pure and needs no ESP-IDF mock (NFR-13).
 * Identity for vectors 1 to 20: host name in use `rgb-tuner-2`, station IP 192.168.1.42 (a renamed device).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "http_portal.h"
#include "mdns_service.h"
}

namespace {

/** IPv4 address in network byte order (first octet first in memory), independent of the host's endianness. */
uint32_t Ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t octets[4] = {a, b, c, d};
    uint32_t address = 0;
    std::memcpy(&address, octets, sizeof(address));
    return address;
}

const char *const kRenamed = "rgb-tuner-2";
const uint32_t kStationIp = Ipv4(192, 168, 1, 42);

bool Allowed(const char *origin) { return IsHttpStationOriginAllowed(origin, kRenamed, kStationIp); }
bool Allowed(const std::string &origin) { return Allowed(origin.c_str()); }

}  // namespace

// ---- Section 7.5 vectors, in table order ----------------------------------------------------------------------------

TEST_CASE("vector 1: an absent Origin is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed(nullptr));
}

TEST_CASE("vector 2: the default name origin is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed("http://rgb-tuner.local"));
}

TEST_CASE("vector 3: the renamed host origin (name in use) is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed("http://rgb-tuner-2.local"));
}

TEST_CASE("vector 4: the station IP origin is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed("http://192.168.1.42"));
}

TEST_CASE("vector 5: the default name with an explicit :80 is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed("http://rgb-tuner.local:80"));
}

TEST_CASE("vector 6: the station IP with an explicit :80 is allowed", "[T-19][FR-27]")
{
    REQUIRE(Allowed("http://192.168.1.42:80"));
}

TEST_CASE("vector 7: the comparison is ASCII case-insensitive", "[T-19][FR-27]")
{
    REQUIRE(Allowed("HTTP://RGB-TUNER.LOCAL"));
}

TEST_CASE("vector 8: the opaque origin null is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("null"));
}

TEST_CASE("vector 9: an empty value is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed(""));
}

TEST_CASE("vector 10: another site is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://evil.example"));
}

TEST_CASE("vector 11: the https scheme is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("https://rgb-tuner.local"));
}

TEST_CASE("vector 12: a port other than 80 is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local:8080"));
}

TEST_CASE("vector 13: an empty port is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local:"));
}

TEST_CASE("vector 14: a trailing slash is foreign (not an origin serialization)", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local/"));
}

TEST_CASE("vector 15: a host that starts with the own name is foreign (suffix attack)", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local.evil.example"));
}

TEST_CASE("vector 16: a host that ends with the own name is foreign (prefix attack)", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://evil-rgb-tuner.local"));
}

TEST_CASE("vector 17: another renamed host is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner-3.local"));
}

TEST_CASE("vector 18: another IP address is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://192.168.1.43"));
}

TEST_CASE("vector 19: the station IP with leading zeros is foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://192.168.001.042"));
}

TEST_CASE("vector 20: a 97-byte value is foreign (longer than HTTP_STATION_ORIGIN_MAX)", "[T-19][FR-27]")
{
    REQUIRE(HTTP_STATION_ORIGIN_MAX == 96);
    const std::string origin = "http://rgb-tuner.local" + std::string(97 - std::strlen("http://rgb-tuner.local"), 'a');
    REQUIRE(origin.size() == 97);
    REQUIRE_FALSE(Allowed(origin));
}

TEST_CASE("vector 21: the renamed origin is foreign when the identity is not renamed", "[T-19][FR-27]")
{
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://rgb-tuner-2.local", "rgb-tuner", kStationIp));
}

TEST_CASE("vector 22: http://0.0.0.0 is foreign when the identity address is unknown (0)", "[T-19][FR-27]")
{
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://0.0.0.0", kRenamed, 0));
}

// ---- Boundaries and combinations around the vectors ----------------------------------------------------------------

TEST_CASE("case-insensitivity also covers the renamed host, the port-less IP and the scheme", "[T-19][FR-27]")
{
    REQUIRE(Allowed("Http://Rgb-Tuner-2.Local"));
    REQUIRE(Allowed("HTTP://RGB-TUNER-2.LOCAL:80"));
    REQUIRE(Allowed("hTtP://192.168.1.42"));
    REQUIRE_FALSE(Allowed("HTTPS://RGB-TUNER.LOCAL"));
}

TEST_CASE("the default name stays allowed after a rename, and without any identity", "[T-19][FR-27]")
{
    REQUIRE(IsHttpStationOriginAllowed("http://rgb-tuner.local", kRenamed, kStationIp));
    REQUIRE(IsHttpStationOriginAllowed("http://rgb-tuner.local", nullptr, 0));
    REQUIRE(IsHttpStationOriginAllowed("http://rgb-tuner.local", "", 0));
    REQUIRE(IsHttpStationOriginAllowed(nullptr, nullptr, 0));
}

TEST_CASE("an unknown or empty name in use matches nothing but the default name", "[T-19][FR-27]")
{
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://.local", "", kStationIp));
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://.local", nullptr, kStationIp));
    REQUIRE(IsHttpStationOriginAllowed("http://192.168.1.42", nullptr, kStationIp));
}

TEST_CASE("an unknown address (0) never matches any IP origin", "[T-19][FR-27]")
{
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://192.168.1.42", kRenamed, 0));
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://0.0.0.0:80", kRenamed, 0));
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://0", kRenamed, 0));
}

TEST_CASE("only exactly :80 is the default port", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local:080"));
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local:8"));
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local:80:80"));
    REQUIRE_FALSE(Allowed("http://192.168.1.42:81"));
    REQUIRE_FALSE(Allowed("http://:80"));
}

TEST_CASE("malformed schemes, hosts and addresses are foreign", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://"));
    REQUIRE_FALSE(Allowed("http:/rgb-tuner.local"));
    REQUIRE_FALSE(Allowed("rgb-tuner.local"));
    REQUIRE_FALSE(Allowed("//rgb-tuner.local"));
    REQUIRE_FALSE(Allowed("ftp://rgb-tuner.local"));
    REQUIRE_FALSE(Allowed(" http://rgb-tuner.local"));
    REQUIRE_FALSE(Allowed("http://rgb-tuner.local "));
    REQUIRE_FALSE(Allowed("http://rgb-tuner"));
    REQUIRE_FALSE(Allowed("http://rgb-tuner.locall"));
    REQUIRE_FALSE(Allowed("http://user@rgb-tuner.local"));
    REQUIRE_FALSE(Allowed("http://192.168.1.42."));
    REQUIRE_FALSE(Allowed("http://192.168.1"));
    REQUIRE_FALSE(Allowed("http://192.168.1.42.1"));
    REQUIRE_FALSE(Allowed("http://192.168.1.420"));
    REQUIRE_FALSE(Allowed("http://192.168.1.0042"));
    REQUIRE_FALSE(Allowed("http://192.168..42"));
    REQUIRE_FALSE(Allowed("http://[::1]"));
}

TEST_CASE("a zero octet is written as a single 0, without leading zeros", "[T-19][FR-27]")
{
    const uint32_t address = Ipv4(10, 0, 0, 7);
    REQUIRE(IsHttpStationOriginAllowed("http://10.0.0.7", kRenamed, address));
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://10.00.0.7", kRenamed, address));
    REQUIRE_FALSE(IsHttpStationOriginAllowed("http://010.0.0.7", kRenamed, address));
    REQUIRE(IsHttpStationOriginAllowed("http://255.255.255.255", kRenamed, Ipv4(255, 255, 255, 255)));
}

TEST_CASE("the octet order is network byte order", "[T-19][FR-27]")
{
    REQUIRE_FALSE(Allowed("http://42.1.168.192"));
}

TEST_CASE("the longest own origin (63-character name, :80) is allowed and fits the 96-byte limit", "[T-19][FR-27]")
{
    const std::string name(MDNS_SERVICE_HOSTNAME_MAX - 1, 'n');   // 63 characters, the DNS label limit
    const std::string origin = "http://" + name + ".local:80";
    REQUIRE(origin.size() == 79);
    REQUIRE(IsHttpStationOriginAllowed(origin.c_str(), name.c_str(), kStationIp));
    REQUIRE_FALSE(IsHttpStationOriginAllowed(("http://" + name + "x.local").c_str(), name.c_str(), kStationIp));
}

TEST_CASE("values of exactly 96 bytes are compared, 97 and more are foreign", "[T-19][FR-27]")
{
    const std::string at_limit = "http://" + std::string(96 - 7, 'a');
    REQUIRE(at_limit.size() == 96);
    REQUIRE_FALSE(Allowed(at_limit));                              // compared in full, just not an own origin
    const std::string long_default = "http://rgb-tuner.local" + std::string(200, ' ');
    REQUIRE_FALSE(Allowed(long_default));
}
