#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

extern "C" {
#include "driver/uart.h"
#include "esp_timer.h"
#include "logging.h"
#include "test_modules.h"
}

namespace {

void ResetFakes()
{
    RESET_FAKE(uart_param_config);
    RESET_FAKE(uart_driver_install);
    RESET_FAKE(uart_write_bytes);
    RESET_FAKE(esp_timer_get_time);
    FFF_RESET_HISTORY();

    uart_param_config_fake.return_val = ESP_OK;
    uart_driver_install_fake.return_val = ESP_OK;
    uart_write_bytes_fake.return_val = 0;
    esp_timer_get_time_fake.return_val = 0;
}

/** Busy-waits (bounded) until at least `min_calls` messages have reached uart_write_bytes(). */
void WaitForUartCallCount(int min_calls, int timeout_ms = 500)
{
    int waited = 0;
    while (uart_write_bytes_fake.call_count < min_calls && waited < timeout_ms) {
        usleep(1000);
        waited++;
    }
}

std::string CapturedLine(int index)
{
    return std::string(static_cast<const char *>(uart_write_bytes_fake.arg1_history[index]),
                        uart_write_bytes_fake.arg2_history[index]);
}

} // namespace

TEST_CASE("LogInit installs the UART0 driver with the interrupt-driven ring buffer", "[FR-6]")
{
    ResetFakes();

    LogInit();

    REQUIRE(uart_param_config_fake.call_count == 1);
    REQUIRE(uart_driver_install_fake.call_count == 1);
    REQUIRE(uart_driver_install_fake.arg0_val == UART_NUM_0);
}

TEST_CASE("A module registered at DEBUG level emits all four levels tagged with its module name",
          "[FR-1][FR-2][FR-3]")
{
    ResetFakes();
    LogInit();

    EmitAllLevelsDebugModule();
    WaitForUartCallCount(4);

    REQUIRE(uart_write_bytes_fake.call_count == 4);
    for (int i = 0; i < uart_write_bytes_fake.call_count; ++i) {
        REQUIRE(CapturedLine(i).find("dbg_module") != std::string::npos);
    }
}

TEST_CASE("A module registered at WARNING level filters out DEBUG and INFO calls at compile time",
          "[FR-3]")
{
    ResetFakes();
    LogInit();

    EmitAllLevelsWarningModule();
    WaitForUartCallCount(2);

    REQUIRE(uart_write_bytes_fake.call_count == 2);
    for (int i = 0; i < uart_write_bytes_fake.call_count; ++i) {
        std::string line = CapturedLine(i);
        bool is_warning_or_error =
            line.find("[WARNING]") != std::string::npos || line.find("[ERROR]") != std::string::npos;
        REQUIRE(is_warning_or_error);
        REQUIRE(line.find("[DEBUG]") == std::string::npos);
        REQUIRE(line.find("[INFO]") == std::string::npos);
    }
}

TEST_CASE("The default build includes a timestamp field derived from esp_timer_get_time()", "[FR-4]")
{
    ResetFakes();
    esp_timer_get_time_fake.return_val = 5000000; /* 5,000,000 us == 5000 ms */
    LogInit();

    EmitAllLevelsDebugModule();
    WaitForUartCallCount(1);

    REQUIRE(uart_write_bytes_fake.call_count >= 1);
    REQUIRE(CapturedLine(0).find("[5000]") != std::string::npos);
}

TEST_CASE("When the queue is full, the oldest message is overwritten in favor of the newest",
          "[FR-7]")
{
    ResetFakes();
    LogInit();

    static std::mutex capture_mutex;
    static std::vector<std::string> captured;
    captured.clear();

    /* Deliberately slow down the consumer so the producer can outpace it and force overflow. */
    uart_write_bytes_fake.custom_fake = [](uart_port_t, const void *buf, size_t len) -> int {
        usleep(20000);
        std::lock_guard<std::mutex> lock(capture_mutex);
        captured.emplace_back(static_cast<const char *>(buf), len);
        return static_cast<int>(len);
    };

    constexpr int kMessageCount = 100;
    for (int i = 0; i < kMessageCount; ++i) {
        EmitSequencedMessage(i);
    }

    usleep(500000); /* Bounded wait for whatever survived to drain. */

    std::lock_guard<std::mutex> lock(capture_mutex);
    REQUIRE_FALSE(captured.empty());
    REQUIRE(captured.size() < kMessageCount); /* Some messages must have been dropped. */
    REQUIRE(captured.back().find("seq=99") != std::string::npos); /* Newest always wins. */
    REQUIRE(captured.front().find("seq=0\n") == std::string::npos); /* Oldest was overwritten. */
}

TEST_CASE("Concurrent tasks logging simultaneously do not corrupt or interleave log lines", "[FR-8]")
{
    ResetFakes();
    LogInit();

    static std::mutex capture_mutex;
    static std::vector<std::string> captured;
    captured.clear();

    uart_write_bytes_fake.custom_fake = [](uart_port_t, const void *buf, size_t len) -> int {
        std::lock_guard<std::mutex> lock(capture_mutex);
        captured.emplace_back(static_cast<const char *>(buf), len);
        return static_cast<int>(len);
    };

    constexpr int kThreadCount = 4;
    constexpr int kMessagesPerThread = 25;
    std::vector<std::thread> producers;
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back([t]() {
            for (int i = 0; i < kMessagesPerThread; ++i) {
                EmitTaggedMessage(t, i);
            }
        });
    }
    for (auto &thread : producers) {
        thread.join();
    }

    int waited = 0;
    while (waited < 2000) {
        {
            std::lock_guard<std::mutex> lock(capture_mutex);
            if (static_cast<int>(captured.size()) >= kThreadCount * kMessagesPerThread) {
                break;
            }
        }
        usleep(1000);
        waited++;
    }

    std::lock_guard<std::mutex> lock(capture_mutex);
    REQUIRE_FALSE(captured.empty());
    for (const auto &line : captured) {
        size_t first = line.find("thread=");
        REQUIRE(first != std::string::npos);
        size_t second = line.find("thread=", first + 1);
        REQUIRE(second == std::string::npos); /* No fragment of a second message. */
    }
}

TEST_CASE("The logging module implementation contains no malloc()/free() calls", "[FR-9]")
{
    std::ifstream file(LOGGING_SOURCE_FILE);
    REQUIRE(file.good());
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    REQUIRE(contents.find("malloc(") == std::string::npos);
    REQUIRE(contents.find("free(") == std::string::npos);
}
