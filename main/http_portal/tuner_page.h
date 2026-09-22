/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file tuner_page.h
 * @brief Embedded WS2812 tuner page (SPEC-003 NFR-1..NFR-3, NFR-17).
 *
 * Private to the http_portal component: not part of the public include/
 * directory, included only by http_portal.c.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Complete `GET /tuner` response body, null-terminated, kept in flash (.rodata). */
extern const char g_tuner_page[];

#ifdef __cplusplus
}
#endif
