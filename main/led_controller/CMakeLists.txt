idf_component_register(SRCS "led_controller.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ws2812_timing
                       PRIV_REQUIRES esp_driver_rmt freertos logging rmt_pulse_monitor)
