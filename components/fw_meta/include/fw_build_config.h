/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_build_config.h
 * @brief Name of the build configuration in effect, for the start log line of both
 * applications (SPEC-007 FR-37).
 *
 * Derived from the optimization Kconfig choice actually compiled in, not from the preset name.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_COMPILER_OPTIMIZATION_DEBUG
/** @brief Build configuration name. */
#define FW_BUILD_CONFIG_NAME "debug (-Og)"
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
#define FW_BUILD_CONFIG_NAME "release (-Os)"
#elif CONFIG_COMPILER_OPTIMIZATION_PERF
#define FW_BUILD_CONFIG_NAME "other (-O2)"
#else
#define FW_BUILD_CONFIG_NAME "other (-O0)"
#endif
