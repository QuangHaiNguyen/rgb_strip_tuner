idf_component_register(SRCS "wifi_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES credential_store
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_hw_support freertos lwip logging)
