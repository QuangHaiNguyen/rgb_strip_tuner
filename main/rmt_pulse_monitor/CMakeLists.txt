idf_component_register(SRCS "rmt_pulse_monitor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ws2812_timing esp_driver_rmt
                       PRIV_REQUIRES freertos logging)
