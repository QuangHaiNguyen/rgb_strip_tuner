idf_component_register(SRCS "fw_update.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES fw_meta esp_partition esp_timer freertos logging)

# SPEC-007 FR-13, FR-14: the embedded metadata takes its version from PROJECT_VER (checked in the root
# CMakeLists.txt), and "-u" keeps the otherwise unreferenced symbol through --gc-sections.
idf_build_get_property(project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_PROJECT_VER="${project_ver}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u g_fw_embedded_meta")
