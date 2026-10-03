#pragma once
/**
 * @file mdns.h
 * @brief Host fake of the managed component's mdns.h (espressif/mdns 1.13.1), SPEC-005 NFR-11.
 *
 * Declares only what main/mdns_service/mdns_service.c uses, with the real signatures. The host tests never
 * depend on managed_components/. MDNS_NAME_MAX_LEN / MDNS_NAME_BUF_LEN have the component's default values
 * (CONFIG_MDNS_RESPOND_REVERSE_QUERIES off). The functions are FFF fakes in mdns_fakes.c.
 */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MDNS_NAME_MAX_LEN 64
#define MDNS_NAME_BUF_LEN (MDNS_NAME_MAX_LEN + 1)

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t mdns_init(void);
void mdns_free(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_hostname_get(char *hostname);
esp_err_t mdns_instance_name_set(const char *instance_name);
esp_err_t mdns_service_add(const char *instance_name, const char *service_type, const char *proto, uint16_t port,
                           mdns_txt_item_t txt[], size_t num_items);
#ifdef __cplusplus
}
#endif
