# SPDX-License-Identifier: CC0-1.0
#
# CMake helpers shared by the firmware project (root) and the updater project (updater/), SPEC-007.

# FR-14: fail the configuration unless the version is exactly MM.mm.pp (8 characters, digits and dots).
function(fw_check_version version)
    if(NOT version MATCHES "^[0-9][0-9]\\.[0-9][0-9]\\.[0-9][0-9]$")
        message(FATAL_ERROR "PROJECT_VER '${version}' is not a valid firmware version: "
                            "expected MM.mm.pp, e.g. 01.00.00 (SPEC-007 FR-14)")
    endif()
endfunction()

# FR-37: print the build configuration derived from the optimization Kconfig choice in effect.
# Call after project(), when the CONFIG_* variables are defined.
function(fw_print_build_config)
    if(CONFIG_COMPILER_OPTIMIZATION_DEBUG)
        set(name "debug (-Og)")
    elseif(CONFIG_COMPILER_OPTIMIZATION_SIZE)
        set(name "release (-Os)")
    elseif(CONFIG_COMPILER_OPTIMIZATION_PERF)
        set(name "other (-O2)")
    else()
        set(name "other (-O0)")
    endif()
    message(STATUS "Build configuration: ${name}")
endfunction()

# FR-3: fail the build if the project binary does not fit the given app partition subtype
# (ESP-IDF's own check only fails when the binary fits no app partition at all).
# Call after project().
function(fw_check_app_partition_size subtype)
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(project_bin PROJECT_BIN)
    partition_table_add_check_size_target(fw_check_size_${subtype}
        DEPENDS gen_project_binary
        BINARY_PATH "${build_dir}/${project_bin}"
        PARTITION_TYPE app
        PARTITION_SUBTYPE ${subtype})
    add_dependencies(app fw_check_size_${subtype})
endfunction()
