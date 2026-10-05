# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_LIST_DIR}/tools/fw_build.cmake)

# SPEC-007 FR-14: firmware version, also used for esp_app_desc_t.version and the embedded metadata.
set(PROJECT_VER "01.00.00")
fw_check_version("${PROJECT_VER}")

set(EXTRA_COMPONENT_DIRS "main/logging" "main/button" "main/credential_store" "main/wifi_manager" "main/dns_server" "main/http_portal" "main/provisioning" "main/ws2812_timing" "main/rmt_pulse_monitor" "main/led_controller" "main/mdns_service" "main/fw_update" "components/fw_meta")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(ExternalProject)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(rgb_strip_tuner)

# SPEC-007 FR-37: configuration print.
fw_print_build_config()

# ---- SPEC-007 FR-2, FR-3, FR-19, FR-36: updater, fw_meta.bin and the flash set ------------------
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(project_bin PROJECT_BIN)
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(idf_target IDF_TARGET)
idf_build_get_property(python PYTHON)

# FR-3: the firmware must fit ota_0 (it is larger than factory by design).
fw_check_app_partition_size(ota_0)

# FR-2: the firmware goes to ota_0. ESP-IDF flashes the project binary to the default boot partition
# (factory, which holds the updater), so its offset in the flash targets is moved to ota_0.
partition_table_get_partition_info(factory_offset "--partition-name factory" "offset")
partition_table_get_partition_info(ota_0_offset "--partition-name ota_0" "offset")
# The project binary's entry in each property, as written by esptool_py_flash_target_image(), for an offset.
function(fw_get_app_flash_entry property offset out_var)
    if(property STREQUAL "IMAGES")
        set(entry "${offset} ${project_bin}")
    elseif(property STREQUAL "FLASH_FILE")
        set(entry "\"${offset}\" : \"${project_bin}\"")
    else()
        set(entry "\"offset\" : \"${offset}\", \"file\" : \"${project_bin}\"")
    endif()
    set(${out_var} "${entry}" PARENT_SCOPE)
endfunction()
foreach(flash_target flash app-flash)
    foreach(property IMAGES FLASH_FILE FLASH_ENTRY)
        get_target_property(entries ${flash_target} ${property})
        fw_get_app_flash_entry(${property} ${factory_offset} factory_entry)
        fw_get_app_flash_entry(${property} ${ota_0_offset} ota_0_entry)
        string(REPLACE "${factory_entry}" "${ota_0_entry}" entries "${entries}")
        set_target_properties(${flash_target} PROPERTIES ${property} "${entries}")
        # Fail the configuration if the rewrite did not apply (e.g. a changed ESP-IDF entry format).
        string(FIND "${entries}" "${ota_0_entry}" ota_0_at)
        string(FIND "${entries}" "${factory_entry}" factory_at)
        if(ota_0_at EQUAL -1 OR NOT factory_at EQUAL -1)
            message(FATAL_ERROR "SPEC-007 FR-2: could not move ${project_bin} to ota_0 (${ota_0_offset}) in "
                                "${flash_target} ${property}: '${entries}'")
        endif()
    endforeach()
endforeach()

# FR-2, FR-36, FR-38: the updater is its own ESP-IDF project, built into this build directory with
# its own sdkconfig. It is always -Os; its assertions follow this project's configuration.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    set(updater_config "release")
else()
    set(updater_config "debug")
endif()
set(updater_dir "${CMAKE_CURRENT_LIST_DIR}/updater")
set(updater_build_dir "${build_dir}/updater")
set(updater_bin "${updater_build_dir}/updater.bin")
externalproject_add(updater
    SOURCE_DIR "${updater_dir}"
    BINARY_DIR "${updater_build_dir}"
    LIST_SEPARATOR |
    CMAKE_ARGS -DSDKCONFIG=${updater_build_dir}/sdkconfig
               -DSDKCONFIG_DEFAULTS=${updater_dir}/sdkconfig.defaults|${updater_dir}/sdkconfig.${updater_config}
               -DIDF_PATH=${idf_path} -DIDF_TARGET=${idf_target}
               -DPYTHON_DEPS_CHECKED=1 -DPYTHON=${python}
    INSTALL_COMMAND ""
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS "${updater_bin}")
esptool_py_flash_to_partition(flash "factory" "${updater_bin}")
add_dependencies(flash updater)

# FR-19: fw_meta.bin, the metadata record of the firmware just built plus a cleared control record.
set(fw_meta_bin "${build_dir}/fw_meta.bin")
set(gen_fw_meta "${CMAKE_CURRENT_LIST_DIR}/tools/gen_fw_meta.py")
# The project binary is generated in another directory scope, so this target runs on every build
# (after gen_project_binary) instead of depending on the .bin file.
add_custom_target(fw_meta_bin ALL
    COMMAND ${python} ${gen_fw_meta} --image "${build_dir}/${project_bin}" --output "${fw_meta_bin}"
    BYPRODUCTS "${fw_meta_bin}"
    VERBATIM)
add_dependencies(fw_meta_bin gen_project_binary)
esptool_py_flash_to_partition(flash "fw_meta" "${fw_meta_bin}")
add_dependencies(flash fw_meta_bin)
