#pragma once
/** @file host_stubs.h @brief Log capture used by all host tests. */
#ifdef __cplusplus
extern "C" {
#endif
/** Log levels as in logging.h: 0 debug, 1 info, 2 warning, 3 error. */
void TestLogReset(void);
/** All captured lines, "[L<level> <module>] text\n" each. */
const char *TestLogText(void);
/** Number of lines captured at @p level. */
int TestLogCount(int level);
#ifdef __cplusplus
}
#endif
