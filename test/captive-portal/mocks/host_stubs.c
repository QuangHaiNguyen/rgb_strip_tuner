/* Log capture (replaces the UART log writer) and esp_err_to_name(). */
#include "host_stubs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "logging.h"

static char s_text[1 << 18];
static size_t s_length;
static int s_counts[4];

void LogWrite(log_level_t level, const char *module_name, const char *fmt, ...)
{
    char line[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (level >= 0 && level < 4) {
        s_counts[level]++;
    }
    int written = snprintf(s_text + s_length, sizeof(s_text) - s_length, "[L%d %s] %s\n", (int)level, module_name, line);
    if (written > 0 && s_length + (size_t)written < sizeof(s_text)) {
        s_length += (size_t)written;
    }
}

void TestLogReset(void)
{
    s_length = 0;
    s_text[0] = '\0';
    memset(s_counts, 0, sizeof(s_counts));
}

const char *TestLogText(void) { return s_text; }
int TestLogCount(int level) { return s_counts[level]; }

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}
