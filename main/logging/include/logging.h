/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logging severity levels, in increasing order of severity.
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
} log_level_t;

/**
 * @brief Maximum length, in characters, of a module name registered via
 * LOG_MODULE_REGISTER(), not including the null terminator.
 *
 * Names longer than this are silently truncated to this length when a log
 * line is emitted. Keep module names within this limit and distinct within
 * their first LOG_MODULE_NAME_MAX_LEN characters to avoid two modules
 * becoming indistinguishable in the log output.
 */
#define LOG_MODULE_NAME_MAX_LEN (15)

/**
 * @brief Initialize the logging module.
 *
 * Installs the UART0 driver and starts the internal log-writer task that
 * drains queued log messages to UART0. Must be called once, before any
 * LOG_DEBUG/INFO/WARNING/ERROR call, typically at the start of app_main().
 * Calling this function more than once has no effect after the first call.
 */
void LogInit(void);

/**
 * @brief Format and queue a log message for transmission.
 *
 * Not intended to be called directly; use the LOG_DEBUG/INFO/WARNING/ERROR
 * macros instead, which apply the calling module's compile-time level
 * filter before invoking this function. Safe to call from multiple tasks
 * concurrently. If LogInit() has not been called yet, this is a no-op.
 *
 * @param level        Severity of the message.
 * @param module_name  Name of the module emitting the message.
 * @param fmt           printf-style format string.
 */
void LogWrite(log_level_t level, const char *module_name, const char *fmt, ...);

/**
 * @brief Register the calling module's name and compile-time minimum log level.
 *
 * Invoke once at file scope, near the top of a source file, before any use
 * of LOG_DEBUG/INFO/WARNING/ERROR in that file. `name` is truncated to
 * LOG_MODULE_NAME_MAX_LEN characters if longer.
 */
#define LOG_MODULE_REGISTER(name, level) \
    static const char *const log_module_name__ = (name); \
    enum { log_module_level__ = (level) }

/** @brief Emit a DEBUG-level log message for the registered module. */
#define LOG_DEBUG(...)   LOG_EMIT_(LOG_LEVEL_DEBUG, __VA_ARGS__)
/** @brief Emit an INFO-level log message for the registered module. */
#define LOG_INFO(...)    LOG_EMIT_(LOG_LEVEL_INFO, __VA_ARGS__)
/** @brief Emit a WARNING-level log message for the registered module. */
#define LOG_WARNING(...) LOG_EMIT_(LOG_LEVEL_WARNING, __VA_ARGS__)
/** @brief Emit an ERROR-level log message for the registered module. */
#define LOG_ERROR(...)   LOG_EMIT_(LOG_LEVEL_ERROR, __VA_ARGS__)

/**
 * @brief Internal helper macro backing LOG_DEBUG/INFO/WARNING/ERROR.
 *
 * `level` and `log_module_level__` are both compile-time constants, so the
 * compiler folds/eliminates the branch (and the call to LogWrite()) entirely
 * when the module's registered level excludes this severity.
 */
#define LOG_EMIT_(level, ...) \
    do { \
        if ((int)(level) >= (int)log_module_level__) { \
            LogWrite((level), log_module_name__, __VA_ARGS__); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif
