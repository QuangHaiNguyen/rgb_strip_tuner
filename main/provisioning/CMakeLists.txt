idf_component_register(SRCS "provisioning.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES button fw_update credential_store wifi_manager dns_server http_portal led_controller mdns_service rmt_pulse_monitor freertos logging)
