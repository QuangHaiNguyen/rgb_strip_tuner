idf_component_register(SRCS "updater_main.c" "updater_ap.c" "updater_http.c" "updater_page.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fw_meta logging app_update bootloader_support esp_partition esp_wifi esp_netif
                                     esp_event esp_http_server esp_hw_support esp_app_format freertos)
