#pragma once
/**
 * @file log_fake_access.h
 * @brief Declares the FFF LogWrite() fake defined in test/ws2812-tuner-page/mocks/log_fakes.c, so
 *        tests can read LogWrite_fake.call_count and find LogWrite in FFF's call history.
 */
#include "fff.h"
#include "logging.h"
#ifdef __cplusplus
extern "C" {
#endif
DECLARE_FAKE_VOID_FUNC_VARARG(LogWrite, log_level_t, const char *, const char *, ...);
#ifdef __cplusplus
}
#endif
