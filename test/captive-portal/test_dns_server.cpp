/**
 * @file test_dns_server.cpp
 * @brief Host tests for the wildcard DNS responder (SPEC-002 T-4; FR-9, FR-20).
 */
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "dns_server.h"
#include "freertos_mock.h"
#include "host_stubs.h"
}

namespace {

const uint32_t kApAddress = htonl(0xC0A80401);  // 192.168.4.1, network byte order

/** Build a one-question query packet. */
std::vector<uint8_t> MakeQuery(const std::string &host, uint16_t type, uint16_t class_in = 1, uint16_t id = 0xBEEF,
                               uint8_t flags_hi = 0x01)
{
    std::vector<uint8_t> packet = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id & 0xFF),
                                   flags_hi, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) {
            dot = host.size();
        }
        packet.push_back(static_cast<uint8_t>(dot - start));
        packet.insert(packet.end(), host.begin() + start, host.begin() + dot);
        start = dot + 1;
    }
    packet.push_back(0);
    packet.push_back(type >> 8);
    packet.push_back(type & 0xFF);
    packet.push_back(class_in >> 8);
    packet.push_back(class_in & 0xFF);
    return packet;
}

std::vector<uint8_t> Respond(const std::vector<uint8_t> &query, size_t buffer_size = 512)
{
    std::vector<uint8_t> response(buffer_size);
    size_t length = BuildDnsResponse(query.data(), query.size(), kApAddress, response.data(), response.size());
    response.resize(length);
    return response;
}

}  // namespace

TEST_CASE("an A query is answered with the AP address", "[T-4][FR-9]")
{
    const auto query = MakeQuery("connectivitycheck.gstatic.com", 1);
    const auto response = Respond(query);

    REQUIRE(response.size() == query.size() + 16);
    REQUIRE(response[0] == 0xBE);                       // ID echoed
    REQUIRE(response[1] == 0xEF);
    REQUIRE((response[2] & 0x80) == 0x80);              // QR: response
    REQUIRE((response[2] & 0x01) == 0x01);              // RD echoed
    REQUIRE((response[3] & 0x0F) == 0);                 // RCODE 0
    REQUIRE(response[7] == 1);                          // ANCOUNT
    REQUIRE(std::memcmp(response.data() + 12, query.data() + 12, query.size() - 12) == 0);  // question echoed

    const uint8_t *answer = response.data() + query.size();
    REQUIRE(answer[0] == 0xC0);                         // name pointer to the question
    REQUIRE(answer[1] == 0x0C);
    REQUIRE(answer[3] == 1);                            // TYPE A
    REQUIRE(answer[5] == 1);                            // CLASS IN
    const uint32_t ttl_s = (answer[6] << 24) | (answer[7] << 16) | (answer[8] << 8) | answer[9];
    REQUIRE(ttl_s <= 60);
    REQUIRE(answer[10] == 0);
    REQUIRE(answer[11] == 4);                           // RDLENGTH
    REQUIRE(std::memcmp(answer + 12, &kApAddress, 4) == 0);
    REQUIRE(answer[12] == 192);
    REQUIRE(answer[15] == 1);
}

TEST_CASE("every hostname resolves to the AP address", "[T-4][FR-9]")
{
    for (const char *host : {"a", "www.example.com", "captive.apple.com", "a.b.c.d.e.f.g.example.org",
                             "connectivity-check.ubuntu.com", "detectportal.firefox.com", "x"}) {
        INFO(host);
        const auto response = Respond(MakeQuery(host, 1));
        REQUIRE_FALSE(response.empty());
        REQUIRE(response[7] == 1);
        REQUIRE(std::memcmp(response.data() + response.size() - 4, &kApAddress, 4) == 0);
    }

    const std::string longest = std::string(63, 'a') + "." + std::string(63, 'b') + ".com";
    REQUIRE(Respond(MakeQuery(longest, 1))[7] == 1);
}

TEST_CASE("non-A queries get an empty NODATA answer", "[T-4][FR-9]")
{
    for (uint16_t type : {28 /*AAAA*/, 15 /*MX*/, 16 /*TXT*/, 5 /*CNAME*/, 12 /*PTR*/, 65 /*HTTPS*/, 255 /*ANY*/}) {
        INFO("type " << type);
        const auto query = MakeQuery("example.com", type);
        const auto response = Respond(query);

        REQUIRE(response.size() == query.size());       // question only, no answer record
        REQUIRE((response[2] & 0x80) == 0x80);
        REQUIRE((response[3] & 0x0F) == 0);             // NOERROR, so clients do not retry as NXDOMAIN
        REQUIRE(response[6] == 0);
        REQUIRE(response[7] == 0);                      // ANCOUNT 0
    }
}

TEST_CASE("an A query of a class other than IN gets NODATA", "[T-4][FR-9]")
{
    const auto response = Respond(MakeQuery("example.com", 1, 3 /*CH*/));
    REQUIRE_FALSE(response.empty());
    REQUIRE(response[7] == 0);
}

TEST_CASE("the reply may reuse the query buffer", "[T-4][FR-9]")
{
    auto packet = MakeQuery("example.com", 1);
    packet.resize(512);
    const size_t query_length = MakeQuery("example.com", 1).size();
    const size_t length = BuildDnsResponse(packet.data(), query_length, kApAddress, packet.data(), packet.size());
    REQUIRE(length == query_length + 16);
    REQUIRE(packet[7] == 1);
    REQUIRE(std::memcmp(&packet[length - 4], &kApAddress, 4) == 0);
}

TEST_CASE("malformed or unsupported packets are dropped", "[T-4][FR-9]")
{
    SECTION("too short") {
        std::vector<uint8_t> tiny(10, 0);
        REQUIRE(BuildDnsResponse(tiny.data(), tiny.size(), kApAddress, tiny.data(), 512) == 0);
    }
    SECTION("already a response") {
        REQUIRE(Respond(MakeQuery("example.com", 1, 1, 1, 0x81)).empty());
    }
    SECTION("no question") {
        auto query = MakeQuery("example.com", 1);
        query[5] = 0;
        REQUIRE(Respond(query).empty());
    }
    SECTION("more than one question") {
        auto query = MakeQuery("example.com", 1);
        query[5] = 2;
        REQUIRE(Respond(query).empty());
    }
    SECTION("label longer than 63") {
        auto query = MakeQuery("example.com", 1);
        query[12] = 64;
        REQUIRE(Respond(query).empty());
    }
    SECTION("name runs past the end of the packet") {
        auto query = MakeQuery("example.com", 1);
        query.resize(query.size() - 6);
        REQUIRE(Respond(query).empty());
    }
    SECTION("missing QTYPE/QCLASS") {
        auto query = MakeQuery("example.com", 1);
        query.resize(query.size() - 3);
        REQUIRE(Respond(query).empty());
    }
    SECTION("reply buffer too small") {
        const auto query = MakeQuery("example.com", 1);
        REQUIRE(Respond(query, query.size() + 15).empty());
        REQUIRE_FALSE(Respond(query, query.size() + 16).empty());
    }
}

TEST_CASE("the DNS task starts and stops without leaking", "[T-8][FR-20][NFR-7]")
{
    MockFreeRtosReset();
    TestLogReset();

    REQUIRE(StartDnsServer(kApAddress));
    REQUIRE(MockGetTaskCount() == 1);
    REQUIRE(std::string(MockGetTaskName(0)) == "dns");

    // The mock task never runs, so mark it deleted the way the real task exits.
    MockMarkTaskDeleted(0);
    StopDnsServer();
    REQUIRE(MockGetNowMs() >= 20);      // waited for the idle task to release the static task memory
    REQUIRE(MockGetNowMs() < 100);

    // Stopping again is harmless and does not wait.
    const uint32_t before_ms = MockGetNowMs();
    StopDnsServer();
    REQUIRE(MockGetNowMs() == before_ms);
}

TEST_CASE("stopping gives up after 2 s if the task never exits", "[T-8][FR-20]")
{
    MockFreeRtosReset();
    REQUIRE(StartDnsServer(kApAddress));

    StopDnsServer();

    REQUIRE(MockGetNowMs() >= 2000);
    REQUIRE(MockGetNowMs() <= 2100);
}

TEST_CASE("starting twice stops the first task before creating another", "[T-8][FR-20]")
{
    MockFreeRtosReset();
    REQUIRE(StartDnsServer(kApAddress));
    MockMarkTaskDeleted(0);
    REQUIRE(StartDnsServer(kApAddress));
    REQUIRE(MockGetTaskCount() == 2);
    MockMarkTaskDeleted(1);
    StopDnsServer();
}
