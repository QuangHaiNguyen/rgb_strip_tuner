# SPEC-007: the updater, the owner's "second stage bootloader". A separate ESP-IDF application in the
# factory partition, built by the root project (ExternalProject) into build/<preset>/updater.
cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_LIST_DIR}/../tools/fw_build.cmake)

set(PROJECT_VER "01.00.00")
fw_check_version("${PROJECT_VER}")

# Shared with the firmware: the logging module (SPEC-001) and the metadata component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../main/logging" "${CMAKE_CURRENT_LIST_DIR}/../components/fw_meta")

if(NOT IDF_PATH)
    set(IDF_PATH "$ENV{IDF_PATH}")
endif()
include(${IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(updater)

# FR-37: configuration print (always release (-Os) for the updater, FR-38).
fw_print_build_config()
# FR-3: the updater must fit the factory partition (832 KB).
fw_check_app_partition_size(factory)
