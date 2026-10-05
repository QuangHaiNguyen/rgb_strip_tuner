idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES spi_flash logging fw_meta fw_update provisioning led_controller rmt_pulse_monitor
                       INCLUDE_DIRS "")
