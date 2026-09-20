/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file dns_server.h
 * @brief Wildcard DNS responder for the captive portal (SPEC-002 FR-9).
 *
 * Every A/IN query is answered with the configured address; queries of any
 * other type get an empty (NODATA) answer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief UDP port the server listens on. */
#define DNS_SERVER_PORT (53)
/** @brief TTL, in seconds, of the A answers (FR-9 allows 0..60). */
#define DNS_SERVER_TTL_S (30)

/**
 * @brief Build the reply for one DNS query packet (pure function).
 *
 * @param[in]  query         Received query packet.
 * @param[in]  query_length  Length of @p query in bytes.
 * @param[in]  ip_addr       Address to answer with, network byte order.
 * @param[out] response      Buffer for the reply; may alias @p query.
 * @param[in]  response_size Size of @p response in bytes.
 * @return Reply length in bytes, or 0 if the packet is not a valid single-question query.
 */
size_t BuildDnsResponse(const uint8_t *query, size_t query_length, uint32_t ip_addr,
                        uint8_t *response, size_t response_size);

/**
 * @brief Start the DNS task.
 *
 * @param ip_addr Address returned for every A query, network byte order.
 * @return true if the task was started.
 */
bool StartDnsServer(uint32_t ip_addr);

/** @brief Stop the DNS task and release its socket. Safe to call when stopped. */
void StopDnsServer(void);

#ifdef __cplusplus
}
#endif
