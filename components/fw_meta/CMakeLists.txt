# SPEC-007: firmware metadata shared by the firmware (root project) and the updater (updater/).
# fw_meta.c is pure (host-testable); fw_meta_flash.c accesses the fw_meta partition.
idf_component_register(SRCS "fw_meta.c" "fw_meta_flash.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition esp_system
                       PRIV_REQUIRES esp_app_format bootloader_support)
