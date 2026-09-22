/* FFF fake of LogWrite(); see log_fakes.h. */
#include "log_fakes.h"
#include "fff.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

DEFINE_FFF_GLOBALS;

/* LogWrite(log_level_t level, const char *module_name, const char *fmt, ...): three fixed
 * arguments plus the printf-style varargs, faked with FFF's variadic support. */
FAKE_VOID_FUNC_VARARG(LogWrite, log_level_t, const char *, const char *, ...);

static char s_text[1 << 16];
static size_t s_length;
static int s_counts[4];

static void CaptureLog(log_level_t level, const char *module_name, const char *fmt, va_list args)
{
    char line[320];
    vsnprintf(line, sizeof(line), fmt, args);
    if (level >= 0 && level < 4) {
        s_counts[level]++;
    }
    int written = snprintf(s_text + s_length, sizeof(s_text) - s_length, "[L%d %s] %s\n",
                            (int)level, module_name, line);
    if (written > 0 && s_length + (size_t)written < sizeof(s_text)) {
        s_length += (size_t)written;
    }
}

void TestLogReset(void)
{
    RESET_FAKE(LogWrite);
    FFF_RESET_HISTORY();
    LogWrite_fake.custom_fake = CaptureLog;
    s_length = 0;
    s_text[0] = '\0';
    memset(s_counts, 0, sizeof(s_counts));
}

const char *TestLogText(void) { return s_text; }
int TestLogCount(int level) { return (level >= 0 && level < 4) ? s_counts[level] : 0; }
