/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file mdns_service.c
 * @brief Thin wrapper around the managed `espressif/mdns` component (SPEC-005 FR-17..FR-21).
 *
 * The managed component allocates its own heap and runs its own task; that is permitted for
 * third-party code (SPEC-005 NFR-6). This file allocates nothing.
 */
#include "mdns_service.h"
#include "logging.h"
#include <string.h>
#include "mdns.h"

LOG_MODULE_REGISTER("mdns_service", LOG_LEVEL_DEBUG);

static bool s_is_running; /* written and read only by the orchestrator task (NFR-9) */

mdns_hostname_status_t ClassifyMdnsHostname(const char *hostname_in_use)
{
    if (hostname_in_use == NULL || hostname_in_use[0] == '\0') {
        return MDNS_HOSTNAME_UNAVAILABLE;
    }
    return strcmp(hostname_in_use, MDNS_SERVICE_HOSTNAME) == 0 ? MDNS_HOSTNAME_DEFAULT : MDNS_HOSTNAME_RENAMED;
}

bool StartMdnsService(void)
{
    if (s_is_running) {
        return true;
    }

    /* mdns_service_add() copies the TXT items, so a stack array is enough. */
    mdns_txt_item_t txt_items[] = {{.key = "path", .value = "/"}};
    const char *step = "init";
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        step = "hostname";
        err = mdns_hostname_set(MDNS_SERVICE_HOSTNAME);
    }
    if (err == ESP_OK) {
        step = "instance";
        err = mdns_instance_name_set(MDNS_SERVICE_INSTANCE);
    }
    if (err == ESP_OK) {
        step = "service";
        err = mdns_service_add(MDNS_SERVICE_INSTANCE, "_http", "_tcp", MDNS_SERVICE_PORT, txt_items,
                               sizeof(txt_items) / sizeof(txt_items[0]));
    }
    if (err != ESP_OK) {
        mdns_free(); /* safe after a partial or failed init */
        LOG_ERROR("mDNS start failed (step=%s err=%d)", step, (int)err);
        return false;
    }

    s_is_running = true;
    LOG_INFO("mDNS started: " MDNS_SERVICE_HOSTNAME ".local, _http._tcp port %d", MDNS_SERVICE_PORT);
    return true;
}

void StopMdnsService(void)
{
    if (!s_is_running) {
        return;
    }
    mdns_free();
    s_is_running = false;
    LOG_INFO("mDNS stopped");
}

bool LogMdnsHostnameInUse(char *hostname_in_use, size_t hostname_size)
{
    /* mdns_hostname_get() needs a buffer of MDNS_NAME_BUF_LEN; the orchestrator stack holds it briefly. */
    char name[MDNS_NAME_BUF_LEN] = {0};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (hostname_in_use != NULL && hostname_size >= MDNS_SERVICE_HOSTNAME_MAX) {
        err = mdns_hostname_get(name);
    }
    if (err == ESP_OK && strlen(name) >= hostname_size) {
        err = ESP_ERR_INVALID_SIZE; /* longer than a DNS label: cannot be one of our names */
    }
    if (err == ESP_OK && ClassifyMdnsHostname(name) == MDNS_HOSTNAME_UNAVAILABLE) {
        err = ESP_ERR_INVALID_STATE; /* the responder reported an empty name */
    }
    if (err != ESP_OK) {
        LOG_WARNING("mDNS hostname unavailable (err=%d)", (int)err);
        return false;
    }

    if (ClassifyMdnsHostname(name) == MDNS_HOSTNAME_DEFAULT) {
        LOG_INFO("mDNS hostname in use: " MDNS_SERVICE_HOSTNAME ".local");
    } else {
        LOG_WARNING("mDNS hostname conflict: using %s.local instead of " MDNS_SERVICE_HOSTNAME ".local", name);
    }
    strcpy(hostname_in_use, name); /* length checked against hostname_size above */
    return true;
}
