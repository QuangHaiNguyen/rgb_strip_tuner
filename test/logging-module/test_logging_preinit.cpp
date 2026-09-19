#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "driver/uart.h"
#include "logging.h"
#include "test_modules.h"
}

TEST_CASE("A log call made before LogInit() is a safe no-op", "[FR-10]")
{
    RESET_FAKE(uart_write_bytes);
    FFF_RESET_HISTORY();

    /* LogInit() is intentionally never called in this test binary. */
    EmitOneMessagePreinit();

    REQUIRE(uart_write_bytes_fake.call_count == 0);
}
