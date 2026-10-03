#pragma once
/* Host mock of esp_netif.h */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; esp_ip4_addr_t netmask; esp_ip4_addr_t gw; } esp_netif_ip_info_t;
typedef struct { union { esp_ip4_addr_t ip4; } u_addr; uint8_t type; } esp_ip_addr_t;
typedef struct { esp_ip_addr_t ip; } esp_netif_dns_info_t;
#define ESP_IPADDR_TYPE_V4 (0U)
typedef enum { ESP_NETIF_OP_SET = 1, ESP_NETIF_OP_GET = 2 } esp_netif_dhcp_option_mode_t;
typedef enum { ESP_NETIF_DOMAIN_NAME_SERVER = 6 } esp_netif_dhcp_option_id_t;
typedef enum { ESP_NETIF_DNS_MAIN = 0 } esp_netif_dns_type_t;
/* SPEC-005 FR-1/FR-2: IP event base, id and payload as in esp_netif_types.h, and the dotted-decimal
 * print helpers as in esp_netif_ip_addr.h (address in network byte order, first octet first in memory). */
typedef enum { IP_EVENT_STA_GOT_IP = 0, IP_EVENT_STA_LOST_IP } ip_event_t;
typedef struct {
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
    bool ip_changed;
} ip_event_got_ip_t;
#define IPSTR "%d.%d.%d.%d"
#define esp_ip4_addr_get_byte(ipaddr, idx) (((const uint8_t *)(&(ipaddr)->addr))[idx])
#define IP2STR(ipaddr) (uint16_t)esp_ip4_addr_get_byte(ipaddr, 0), (uint16_t)esp_ip4_addr_get_byte(ipaddr, 1), \
                       (uint16_t)esp_ip4_addr_get_byte(ipaddr, 2), (uint16_t)esp_ip4_addr_get_byte(ipaddr, 3)
#ifdef __cplusplus
extern "C" {
#endif
extern esp_event_base_t IP_EVENT;
esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info);
esp_err_t esp_netif_dhcps_option(esp_netif_t *netif, esp_netif_dhcp_option_mode_t mode,
                                 esp_netif_dhcp_option_id_t id, void *value, uint32_t length);
esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type, esp_netif_dns_info_t *dns);
#ifdef __cplusplus
}
#endif
