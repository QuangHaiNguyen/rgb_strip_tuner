idf_component_register(SRCS "logging.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart esp_timer freertos)
