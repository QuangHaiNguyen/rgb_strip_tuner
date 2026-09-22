idf_component_register(SRCS "http_portal.c" "portal_form.c" "tuner_page.c"
                       INCLUDE_DIRS "include"
                       REQUIRES credential_store wifi_manager ws2812_timing
                       PRIV_REQUIRES esp_http_server freertos logging)
