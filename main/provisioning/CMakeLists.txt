idf_component_register(SRCS "provisioning.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES button credential_store wifi_manager dns_server http_portal freertos logging)
