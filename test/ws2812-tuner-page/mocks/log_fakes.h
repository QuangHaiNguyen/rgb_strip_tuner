#pragma once
/**
 * @file log_fakes.h
 * @brief FFF fake of LogWrite() (SPEC-003 T-2, NFR-15), replacing the UART log writer on the host.
 *
 * Unlike test/captive-portal/mocks/host_stubs.c (a hand-written LogWrite substitute), this fake
 * is built on FFF's variadic-function support so LogWs2812Timing()/LogWs2812Rejection() and any
 * other LOG_* call reachable from the code under test can be exercised and captured without a
 * hardware UART.
 */
#include "logging.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Reset the fake and the captured log text/counters. Call at the start of every TEST_CASE. */
void TestLogReset(void);
/** All captured lines since the last reset, one "[L<level> <module>] text\n" line per call. */
const char *TestLogText(void);
/** Number of LogWrite() calls captured at @p level (0 debug, 1 info, 2 warning, 3 error). */
int TestLogCount(int level);

#ifdef __cplusplus
}
#endif
