/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file tuner_page.h
 * @brief Embedded WS2812 tuner pages (SPEC-003 NFR-1..NFR-3, NFR-17; SPEC-005 FR-15).
 *
 * Private to the http_portal component: not part of the public include/
 * directory, included only by http_portal.c.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Complete provisioning-profile `GET /tuner` response body, null-terminated, kept in flash (.rodata). */
extern const char g_tuner_page[];

/**
 * @brief Station-profile `GET /` and `GET /tuner` response body (SPEC-005 FR-15).
 *
 * Byte-for-byte g_tuner_page without the `<a href=/>Back</a>` element; null-terminated, in flash (.rodata).
 */
extern const char g_tuner_page_station[];

#ifdef __cplusplus
}
#endif
