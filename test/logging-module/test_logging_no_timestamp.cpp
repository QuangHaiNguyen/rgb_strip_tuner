#include <catch2/catch_test_macros.hpp>

#include <string>

#include <unistd.h>

extern "C" {
#include "driver/uart.h"
#include "logging.h"
#include "test_modules.h"
}

TEST_CASE("Disabling CONFIG_LOG_TIMESTAMP_ENABLE removes the timestamp field from the log line",
          "[FR-5]")
{
    RESET_FAKE(uart_param_config);
    RESET_FAKE(uart_driver_install);
    RESET_FAKE(uart_write_bytes);
    FFF_RESET_HISTORY();
    uart_param_config_fake.return_val = ESP_OK;
    uart_driver_install_fake.return_val = ESP_OK;

    LogInit();
    EmitOneMessageNoTimestamp();

    int waited = 0;
    while (uart_write_bytes_fake.call_count < 1 && waited < 500) {
        usleep(1000);
        waited++;
    }

    REQUIRE(uart_write_bytes_fake.call_count >= 1);
    std::string line(static_cast<const char *>(uart_write_bytes_fake.arg1_val),
                      uart_write_bytes_fake.arg2_val);
    /* Format without a timestamp is "[LEVEL][module] message\n", so it starts with the level. */
    REQUIRE(line.rfind("[INFO][", 0) == 0);
}
