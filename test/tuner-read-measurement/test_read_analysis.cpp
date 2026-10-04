/**
 * @file test_read_analysis.cpp
 * @brief SPEC-006 T-1 (FR-15, NFR-8): AnalyzeWs2812Read() against the section 7.2 reference vectors R1 to R11 and the
 *        FR-15 boundary rules (usable rules, final-symbol exclusion, split threshold and tie, single-class fallback at
 *        625 ns, round-half-up, not-found zeros, minimum 8, empty input, overflow bounds).
 *
 * rmt_pulse_monitor.c is compiled as-is; the RMT/FreeRTOS FFF fakes only satisfy the linker, and their call counts
 * confirm the function touches no driver (NFR-8).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fff.h"
extern "C" {
#include "freertos_fakes.h"
#include "log_fake_access.h"
#include "log_fakes.h"
#include "rmt_fakes.h"
#include "rmt_pulse_monitor.h"
}

namespace {

constexpr uint32_t kTickNs = 25;

rmt_symbol_word_t Sym(uint32_t d0, uint32_t d1, uint32_t level0 = 1, uint32_t level1 = 0)
{
    rmt_symbol_word_t symbol{};
    symbol.duration0 = d0;
    symbol.level0 = level0;
    symbol.duration1 = d1;
    symbol.level1 = level1;
    return symbol;
}

void Append(std::vector<rmt_symbol_word_t> &symbols, int count, uint32_t d0, uint32_t d1)
{
    for (int index = 0; index < count; ++index) {
        symbols.push_back(Sym(d0, d1));
    }
}

struct Result {
    bool ok;
    ws2812_read_stats_t stats;
};

Result Analyze(const std::vector<rmt_symbol_word_t> &symbols, uint32_t tick_ns = kTickNs)
{
    Result result{};
    std::memset(&result.stats, 0xA5, sizeof(result.stats));   // garbage: the function must zero it first
    result.ok = AnalyzeWs2812Read(symbols.empty() ? nullptr : symbols.data(), symbols.size(), tick_ns, &result.stats);
    return result;
}

void RequireBit0(const ws2812_read_stats_t &stats, uint32_t count, uint32_t high, uint32_t period)
{
    CHECK(stats.bit0_count == count);
    CHECK(stats.bit0_high_avg_ns == high);
    CHECK(stats.bit0_period_avg_ns == period);
}

void RequireBit1(const ws2812_read_stats_t &stats, uint32_t count, uint32_t high, uint32_t period)
{
    CHECK(stats.bit1_count == count);
    CHECK(stats.bit1_high_avg_ns == high);
    CHECK(stats.bit1_period_avg_ns == period);
}

std::string ReadSource(const char *path)
{
    std::ifstream file(path);
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST_CASE("constants of section 7.1 for the read analysis", "[T-1][FR-14][FR-15][NFR-4]")
{
    REQUIRE(RMT_PULSE_MONITOR_READ_MIN_BITS == 8);
    REQUIRE(RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS == 100);
    REQUIRE(RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS == 625);
    REQUIRE(RMT_PULSE_MONITOR_READ_TIMEOUT_MS == 1000);
    REQUIRE(pdMS_TO_TICKS(RMT_PULSE_MONITOR_READ_TIMEOUT_MS) == 100);
    REQUIRE(sizeof(ws2812_read_stats_t) == 28);
}

TEST_CASE("R1: 143 alternating bits + final -> two classes split at 600 ns", "[T-1][FR-15][R1]")
{
    std::vector<rmt_symbol_word_t> symbols;
    for (int index = 0; index < 143; ++index) {
        symbols.push_back(index % 2 == 0 ? Sym(16, 34) : Sym(32, 18));
    }
    symbols.push_back(Sym(16, 0));
    REQUIRE(symbols.size() == 144);

    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 143);
    RequireBit0(result.stats, 72, 400, 1250);
    RequireBit1(result.stats, 71, 800, 1250);
}

TEST_CASE("R2: 20 x bit-0 shapes -> one class bit 0, bit 1 not found", "[T-1][FR-15][R2]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 20, 16, 34);
    symbols.push_back(Sym(16, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 20);
    RequireBit0(result.stats, 20, 400, 1250);
    RequireBit1(result.stats, 0, 0, 0);
}

TEST_CASE("R3: 20 x bit-1 shapes -> one class bit 1, bit 0 not found", "[T-1][FR-15][R3]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 20, 32, 18);
    symbols.push_back(Sym(32, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 20);
    RequireBit0(result.stats, 0, 0, 0);
    RequireBit1(result.stats, 20, 800, 1250);
}

TEST_CASE("R4: 7 usable symbols -> false, usable 7", "[T-1][FR-15][R4]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 7, 16, 34);
    symbols.push_back(Sym(16, 0));
    const Result result = Analyze(symbols);
    REQUIRE_FALSE(result.ok);
    CHECK(result.stats.usable_count == 7);
}

TEST_CASE("minimum: exactly 8 usable symbols -> true", "[T-1][FR-15][FR-14]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 8, 16, 34);
    symbols.push_back(Sym(16, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 8);
}

TEST_CASE("R5: a first symbol starting mid-pulse (level0 = 0) is excluded", "[T-1][FR-15][R5]")
{
    std::vector<rmt_symbol_word_t> symbols;
    symbols.push_back(Sym(12, 20, 0, 1));
    Append(symbols, 10, 16, 34);
    Append(symbols, 10, 32, 18);
    symbols.push_back(Sym(32, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 20);
    RequireBit0(result.stats, 10, 400, 1250);
    RequireBit1(result.stats, 10, 800, 1250);
}

TEST_CASE("R6: spread 75 ns < 100 -> one class bit 0, average rounded half up to 388", "[T-1][FR-15][R6]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 14, 36);
    Append(symbols, 10, 17, 33);
    symbols.push_back(Sym(17, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 20);
    RequireBit0(result.stats, 20, 388, 1250);
    RequireBit1(result.stats, 0, 0, 0);
}

TEST_CASE("R7: spread exactly 100 ns -> two classes, 400 is bit 0 and 500 is bit 1", "[T-1][FR-15][R7]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 16, 34);
    Append(symbols, 10, 20, 30);
    symbols.push_back(Sym(20, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    RequireBit0(result.stats, 10, 400, 1250);
    RequireBit1(result.stats, 10, 500, 1250);
}

TEST_CASE("R8: a symbol exactly at the 600 ns midpoint is bit 0 (tie rule 2h <= min+max)", "[T-1][FR-15][R8]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 16, 34);
    symbols.push_back(Sym(24, 26));
    Append(symbols, 10, 32, 18);
    symbols.push_back(Sym(32, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 21);
    RequireBit0(result.stats, 11, 418, 1250);   // (10*400 + 600 + 5) / 11 = 418
    RequireBit1(result.stats, 10, 800, 1250);
}

TEST_CASE("R9: a symbol with duration1 = 0 in the middle is excluded", "[T-1][FR-15][R9]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 16, 34);
    symbols[5].duration1 = 0;
    Append(symbols, 10, 32, 18);
    symbols.push_back(Sym(32, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 19);
    RequireBit0(result.stats, 9, 400, 1250);
    RequireBit1(result.stats, 10, 800, 1250);
}

TEST_CASE("usable rules: level1 = 1 and duration0 = 0 are excluded as well", "[T-1][FR-15]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 16, 34);
    symbols[2] = Sym(16, 34, 1, 1);   // level1 = 1
    symbols[3] = Sym(0, 34);          // duration0 = 0
    symbols[4] = Sym(16, 34, 0, 0);   // level0 = 0 in the middle
    symbols.push_back(Sym(16, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok == false);      // 7 usable
    CHECK(result.stats.usable_count == 7);
}

TEST_CASE("final-symbol exclusion: the last symbol is never usable, even with a valid low", "[T-1][FR-15]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 9, 16, 34);
    symbols.push_back(Sym(32, 18));   // valid shape, but last: would be bit 1 if counted
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 9);
    RequireBit0(result.stats, 9, 400, 1250);
    RequireBit1(result.stats, 0, 0, 0);
}

TEST_CASE("R10: symbol_count 1 or 0 -> false, usable 0, stats zeroed, no out-of-bounds access", "[T-1][FR-15][R10]")
{
    SECTION("one symbol (valid shape, but final)")
    {
        const std::vector<rmt_symbol_word_t> symbols{Sym(16, 34)};   // exactly one element allocated
        const Result result = Analyze(symbols);
        REQUIRE_FALSE(result.ok);
        CHECK(result.stats.usable_count == 0);
        RequireBit0(result.stats, 0, 0, 0);
        RequireBit1(result.stats, 0, 0, 0);
    }
    SECTION("zero symbols, NULL pointer")
    {
        const Result result = Analyze({});
        REQUIRE_FALSE(result.ok);
        CHECK(result.stats.usable_count == 0);
        RequireBit0(result.stats, 0, 0, 0);
        RequireBit1(result.stats, 0, 0, 0);
    }
    SECTION("zero symbols, non-NULL pointer")
    {
        const rmt_symbol_word_t one = Sym(16, 34);
        ws2812_read_stats_t stats;
        std::memset(&stats, 0xA5, sizeof(stats));
        REQUIRE_FALSE(AnalyzeWs2812Read(&one, 0, kTickNs, &stats));
        CHECK(stats.usable_count == 0);
        CHECK(stats.bit0_high_avg_ns == 0);
        CHECK(stats.bit1_period_avg_ns == 0);
    }
}

TEST_CASE("R11: 143 x (32767, 32767) -> no overflow, high 819,175 and period 1,638,350", "[T-1][FR-15][R11]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 143, 32767, 32767);
    symbols.push_back(Sym(32767, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    CHECK(result.stats.usable_count == 143);
    // Spread 0, 819,175 * 2 > 1,250 -> one class bit 1.
    RequireBit0(result.stats, 0, 0, 0);
    RequireBit1(result.stats, 143, 819175, 1638350);
}

TEST_CASE("single-class fallback boundary at 625 ns: 625 is bit 0, 650 is bit 1", "[T-1][FR-15]")
{
    SECTION("all highs 625 ns: min + max = 1,250 <= 1,250 -> bit 0")
    {
        std::vector<rmt_symbol_word_t> symbols;
        Append(symbols, 10, 25, 25);
        symbols.push_back(Sym(25, 0));
        const Result result = Analyze(symbols);
        REQUIRE(result.ok);
        RequireBit0(result.stats, 10, 625, 1250);
        RequireBit1(result.stats, 0, 0, 0);
    }
    SECTION("all highs 650 ns: min + max = 1,300 > 1,250 -> bit 1")
    {
        std::vector<rmt_symbol_word_t> symbols;
        Append(symbols, 10, 26, 24);
        symbols.push_back(Sym(26, 0));
        const Result result = Analyze(symbols);
        REQUIRE(result.ok);
        RequireBit0(result.stats, 0, 0, 0);
        RequireBit1(result.stats, 10, 650, 1250);
    }
    SECTION("600 vs 650 ns (spread 50): min + max = 1,250 -> both in bit 0")
    {
        std::vector<rmt_symbol_word_t> symbols;
        Append(symbols, 5, 24, 26);
        Append(symbols, 5, 26, 24);
        symbols.push_back(Sym(26, 0));
        const Result result = Analyze(symbols);
        REQUIRE(result.ok);
        RequireBit0(result.stats, 10, 625, 1250);
        RequireBit1(result.stats, 0, 0, 0);
    }
}

TEST_CASE("round half up applies to the period average too", "[T-1][FR-15]")
{
    // Periods 1,250 and 1,275 ns, 4 each -> 1,262.5 -> 1,263. Highs all 400 ns.
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 4, 16, 34);
    Append(symbols, 4, 16, 35);
    symbols.push_back(Sym(16, 0));
    const Result result = Analyze(symbols);
    REQUIRE(result.ok);
    RequireBit0(result.stats, 8, 400, 1263);
}

TEST_CASE("tick_ns is honored (no hard-coded 25 ns)", "[T-1][FR-15]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 40, 85);    // at 10 ns/tick: 400 / 1,250
    Append(symbols, 10, 80, 45);    // 800 / 1,250
    symbols.push_back(Sym(80, 0));
    const Result result = Analyze(symbols, 10);
    REQUIRE(result.ok);
    RequireBit0(result.stats, 10, 400, 1250);
    RequireBit1(result.stats, 10, 800, 1250);
}

TEST_CASE("the input is not modified and a NULL stats pointer is rejected without a crash", "[T-1][FR-15]")
{
    std::vector<rmt_symbol_word_t> symbols;
    Append(symbols, 10, 16, 34);
    symbols.push_back(Sym(16, 0));
    const std::vector<rmt_symbol_word_t> copy = symbols;
    (void)Analyze(symbols);
    REQUIRE(std::memcmp(copy.data(), symbols.data(), symbols.size() * sizeof(rmt_symbol_word_t)) == 0);
    REQUIRE_FALSE(AnalyzeWs2812Read(symbols.data(), symbols.size(), kTickNs, nullptr));
}

TEST_CASE("NFR-8: AnalyzeWs2812Read calls no driver, no FreeRTOS and no log function", "[T-1][NFR-8]")
{
    TestLogReset();
    FreeRtosFakesReset();
    RmtFakesReset();
    FFF_RESET_HISTORY();
    std::vector<rmt_symbol_word_t> symbols;
    for (int index = 0; index < 143; ++index) {
        symbols.push_back(index % 2 == 0 ? Sym(16, 34) : Sym(32, 18));
    }
    symbols.push_back(Sym(16, 0));
    (void)Analyze(symbols);
    REQUIRE(fff.call_history_idx == 0);
    REQUIRE(LogWrite_fake.call_count == 0);
    REQUIRE(rmt_receive_fake.call_count == 0);
    REQUIRE(xSemaphoreTake_fake.call_count == 0);

    // Static view of the same: the function body names no driver/RTOS/log API.
    const std::string source = ReadSource(RMT_PULSE_MONITOR_SRC);
    const size_t begin = source.find("bool AnalyzeWs2812Read(");
    REQUIRE(begin != std::string::npos);
    const size_t end = source.find("\n}\n", begin);
    REQUIRE(end != std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    for (const char *token : {"rmt_receive", "rmt_enable", "rmt_disable", "rmt_transmit", "xSemaphore", "xQueue", "LOG_",
                              "malloc", "free(", "float", "double"}) {
        INFO(token);
        REQUIRE(body.find(token) == std::string::npos);
    }
}
