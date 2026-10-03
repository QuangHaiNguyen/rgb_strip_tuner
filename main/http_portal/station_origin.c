/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file station_origin.c
 * @brief Pure `Origin` check for station-mode `POST /tuner` (SPEC-005 FR-27, section 7.5).
 *
 * No allocation, no copy of the inputs, no formatting: the value is compared in place,
 * bounded by HTTP_STATION_ORIGIN_MAX (SPEC-005 NFR-16).
 */
#include "http_portal.h"
#include "mdns_service.h"
#include <string.h>
#include <strings.h>

#define ORIGIN_SCHEME "http://"
#define ORIGIN_SCHEME_LENGTH (sizeof(ORIGIN_SCHEME) - 1)
#define ORIGIN_DEFAULT_PORT ":80"
#define ORIGIN_DEFAULT_PORT_LENGTH (sizeof(ORIGIN_DEFAULT_PORT) - 1)
#define ORIGIN_LOCAL_SUFFIX ".local"
#define ORIGIN_LOCAL_SUFFIX_LENGTH (sizeof(ORIGIN_LOCAL_SUFFIX) - 1)
#define IPV4_OCTET_COUNT (4)
#define IPV4_OCTET_DIGITS_MAX (3)

/**
 * @brief Compare a host with `<hostname>.local`, ASCII case-insensitively and in full.
 *
 * @param host        Host part of the origin (not null-terminated).
 * @param host_length Length of @p host.
 * @param hostname    Name without `.local`; a name that is empty or not terminated within
 *                    MDNS_SERVICE_HOSTNAME_MAX bytes never matches.
 * @return true on a match.
 */
static bool IsLocalHostMatch(const char *host, size_t host_length, const char *hostname)
{
    if (hostname == NULL) {
        return false;
    }
    size_t name_length = strnlen(hostname, MDNS_SERVICE_HOSTNAME_MAX);
    if (name_length == 0 || name_length == MDNS_SERVICE_HOSTNAME_MAX ||
        host_length != name_length + ORIGIN_LOCAL_SUFFIX_LENGTH) {
        return false;
    }
    return strncasecmp(host, hostname, name_length) == 0 &&
           strncasecmp(&host[name_length], ORIGIN_LOCAL_SUFFIX, ORIGIN_LOCAL_SUFFIX_LENGTH) == 0;
}

/**
 * @brief Compare a host with the dotted-decimal form of an IPv4 address, without leading zeros.
 *
 * @param host         Host part of the origin (not null-terminated).
 * @param host_length  Length of @p host.
 * @param station_ipv4 Address in network byte order (first octet first in memory); 0 never matches.
 * @return true on a match.
 */
static bool IsAddressHostMatch(const char *host, size_t host_length, uint32_t station_ipv4)
{
    if (station_ipv4 == 0) {
        return false;
    }
    const uint8_t *octets = (const uint8_t *)&station_ipv4;
    size_t position = 0;
    for (size_t index = 0; index < IPV4_OCTET_COUNT; ++index) {
        if (index > 0) {
            if (position >= host_length || host[position] != '.') {
                return false;
            }
            ++position;
        }
        size_t start = position;
        uint32_t value = 0;
        while (position < host_length && position - start < IPV4_OCTET_DIGITS_MAX &&
               host[position] >= '0' && host[position] <= '9') {
            value = value * 10 + (uint32_t)(host[position] - '0');
            ++position;
        }
        size_t digit_count = position - start;
        bool has_leading_zero = (digit_count > 1 && host[start] == '0');
        if (digit_count == 0 || has_leading_zero || value != octets[index]) {
            return false;
        }
    }
    return position == host_length;
}

bool IsHttpStationOriginAllowed(const char *origin, const char *hostname_in_use, uint32_t station_ipv4)
{
    if (origin == NULL) {
        return true; /* absent header: curl and older browsers (FR-27 a) */
    }
    size_t origin_length = strnlen(origin, HTTP_STATION_ORIGIN_MAX + 1);
    if (origin_length > HTTP_STATION_ORIGIN_MAX || origin_length <= ORIGIN_SCHEME_LENGTH ||
        strncasecmp(origin, ORIGIN_SCHEME, ORIGIN_SCHEME_LENGTH) != 0) {
        return false;
    }

    const char *host = &origin[ORIGIN_SCHEME_LENGTH];
    size_t host_length = origin_length - ORIGIN_SCHEME_LENGTH;
    if (host_length > ORIGIN_DEFAULT_PORT_LENGTH &&
        strcmp(&host[host_length - ORIGIN_DEFAULT_PORT_LENGTH], ORIGIN_DEFAULT_PORT) == 0) {
        host_length -= ORIGIN_DEFAULT_PORT_LENGTH; /* explicit default port is the same origin (FR-27 c) */
    }

    return IsLocalHostMatch(host, host_length, MDNS_SERVICE_HOSTNAME) ||
           IsLocalHostMatch(host, host_length, hostname_in_use) ||
           IsAddressHostMatch(host, host_length, station_ipv4);
}
