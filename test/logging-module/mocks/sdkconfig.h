#pragma once

/* Host-test defaults for the logging module's Kconfig options. Individual
 * test targets may override CONFIG_LOG_TIMESTAMP_ENABLE via a compiler
 * definition (see test/logging-module/CMakeLists.txt). */

#ifndef CONFIG_LOG_TIMESTAMP_ENABLE
#define CONFIG_LOG_TIMESTAMP_ENABLE 1
#endif

#ifndef CONFIG_LOG_UART_BAUD_RATE
#define CONFIG_LOG_UART_BAUD_RATE 115200
#endif

#ifndef CONFIG_LOG_UART_TX_RING_BUFFER_SIZE
#define CONFIG_LOG_UART_TX_RING_BUFFER_SIZE 1024
#endif
