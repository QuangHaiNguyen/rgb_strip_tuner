cmake_minimum_required(VERSION 3.16)
project(firmware_updater_tests C CXX)

# Host tests for SPEC-007 (docs/specs/firmware-updater.md, "Firmware updater: browser upload over an open SoftAP"):
# the pure fw_meta logic (T-1 to T-3, T-18), the fw_meta flash accessors and the firmware's fw_update component (T-3,
# T-5) on an in-memory NOR-flash model, the update gesture (T-4), the orchestrator's MSG_UPDATE_REQUEST and healthy
# deadline (T-5), the updater upload handler (T-6), the updater page (T-7 static part), the updater boot flow on fakes,
# and tools/gen_fw_meta.py against the C serializer (T-3). Standalone CMake project, independent of the ESP-IDF build.
#
# Reused infrastructure:
# - third_party/catch2, third_party/fff (git submodules);
# - test/captive-portal/mocks: httpd simulator, fake FreeRTOS clock, log capture (host_stubs.c), GPIO fakes, and the
#   provisioning harness + orchestrator component fakes (which already fake RequestFwUpdate, RestartIntoUpdater and
#   MarkFirmwareHealthy).
# mocks/ adds the SPEC-007 ESP-IDF header mocks (esp_partition, esp_ota_ops, esp_sleep, bootloader_common, ...), the
# FFF fakes with the NOR-flash model (fw_flash_fakes.c) and the harnesses that compile updater/main/*.c into the tests.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ucontext (freertos_mock) needs the GNU dialect
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(FW_META_DIR ${REPO_ROOT}/components/fw_meta)
set(UPDATER_DIR ${REPO_ROOT}/updater/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(PORTAL_MOCK_DIR ${REPO_ROOT}/test/captive-portal/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)

# Component headers used by the orchestrator and button binaries (as in the captive-portal suite).
set(COMPONENT_INCLUDES
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/button/include
    ${MAIN_DIR}/fw_update/include
    ${MAIN_DIR}/credential_store/include
    ${MAIN_DIR}/wifi_manager/include
    ${MAIN_DIR}/dns_server/include
    ${MAIN_DIR}/http_portal/include
    ${MAIN_DIR}/provisioning/include
    ${MAIN_DIR}/ws2812_timing/include
    ${MAIN_DIR}/led_controller/include
    ${MAIN_DIR}/mdns_service/include
    ${MAIN_DIR}/rmt_pulse_monitor/include)

# SPEC-007 sources: MOCK_DIR comes first so its esp_err.h / esp_system.h / esp_http_server.h (supersets of the
# captive-portal mocks) replace the ESP-IDF headers; PORTAL_MOCK_DIR supplies freertos/*.h and the httpd simulator.
set(FW_INCLUDES
    ${MOCK_DIR}
    ${PORTAL_MOCK_DIR}
    ${FFF_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FW_META_DIR}/include
    ${UPDATER_DIR}
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/fw_update/include)

set(FW_META_SRCS ${FW_META_DIR}/fw_meta.c)
set(FW_FLASH_SRCS ${FW_META_DIR}/fw_meta_flash.c ${MOCK_DIR}/fw_flash_fakes.c ${MOCK_DIR}/fff_globals.c
    ${PORTAL_MOCK_DIR}/host_stubs.c)

function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

# ---- T-1, T-18, T-2, T-3 (C side): pure fw_meta functions (NFR-8) ------------------------------------------------------
add_feature_test(fw_meta_pure_tests
    test_fw_boot_decision.cpp
    test_fw_meta_records.cpp
    test_fw_meta_cxx.cpp
    ${FW_META_SRCS})
target_include_directories(fw_meta_pure_tests PRIVATE ${FW_INCLUDES})

# ---- T-3 (flash accessors), T-5 (fw_update): fw_meta_flash.c and fw_update.c on the NOR-flash model -------------------
add_feature_test(fw_meta_flash_tests
    test_fw_meta_flash.cpp
    test_fw_update.cpp
    ${FW_META_SRCS}
    ${FW_FLASH_SRCS}
    ${MAIN_DIR}/fw_update/fw_update.c)
target_include_directories(fw_meta_flash_tests PRIVATE ${FW_INCLUDES})
# fw_update.c takes the embedded version from PROJECT_VER (main/fw_update/CMakeLists.txt); a test value is used here.
target_compile_definitions(fw_meta_flash_tests PRIVATE FW_PROJECT_VER="01.02.03" FW_PROJECT_VER_FOR_TEST="01.02.03")

# ---- T-6, T-7 static: updater upload handler, status, 404 and page (updater_http.c via mocks/updater_http_harness.c) --
add_feature_test(updater_http_tests
    test_updater_upload.cpp
    test_updater_page.cpp
    test_updater_concurrency.cpp
    ${MOCK_DIR}/updater_http_harness.c
    ${PORTAL_MOCK_DIR}/httpd_mock.c
    ${UPDATER_DIR}/updater_page.c
    ${FW_META_SRCS}
    ${FW_FLASH_SRCS})
target_include_directories(updater_http_tests PRIVATE ${FW_INCLUDES})

# ---- T-1/T-18 at the updater level, FR-6/7/11/17/40 host part: updater_main.c via mocks/updater_main_harness.c ------
add_feature_test(updater_boot_tests
    test_updater_boot.cpp
    ${MOCK_DIR}/updater_main_harness.c
    ${UPDATER_DIR}/updater_page.c
    ${FW_META_SRCS}
    ${FW_FLASH_SRCS})
target_include_directories(updater_boot_tests PRIVATE ${FW_INCLUDES})

# ---- T-4: update gesture (button.c with the captive-portal GPIO fakes and fake FreeRTOS clock) ------------------------
add_feature_test(update_gesture_tests
    test_update_gesture.cpp
    ${MAIN_DIR}/button/button.c
    ${PORTAL_MOCK_DIR}/gpio_fakes.c
    ${PORTAL_MOCK_DIR}/freertos_mock.c
    ${PORTAL_MOCK_DIR}/host_stubs.c)
target_include_directories(update_gesture_tests PRIVATE ${PORTAL_MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

# ---- T-5 orchestrator part: provisioning.c with every component faked (captive-portal harness and fakes) -------------
add_feature_test(update_orchestrator_tests
    test_update_orchestrator.cpp
    ${PORTAL_MOCK_DIR}/provisioning_harness.c
    ${PORTAL_MOCK_DIR}/provisioning_fakes.c
    ${PORTAL_MOCK_DIR}/freertos_mock.c
    ${PORTAL_MOCK_DIR}/host_stubs.c)
target_include_directories(update_orchestrator_tests PRIVATE ${PORTAL_MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

# ---- Static source checks (FR-4/FR-8 firmware side, FR-33, FR-40, FR-41) -------------------------------------------
add_feature_test(fw_static_tests test_fw_static.cpp)
target_compile_definitions(fw_static_tests PRIVATE REPO_ROOT="${REPO_ROOT}")

# ---- T-3 host script: tools/gen_fw_meta.py against the C serializer --------------------------------------------------
# Without python3 the test is registered as skipped (exit code 77), never silently passed.
find_program(PYTHON3_EXECUTABLE NAMES python3)
if(PYTHON3_EXECUTABLE)
    add_feature_test(gen_fw_meta_tests test_gen_fw_meta.cpp ${FW_META_SRCS})
    target_include_directories(gen_fw_meta_tests PRIVATE ${FW_INCLUDES})
    target_compile_definitions(gen_fw_meta_tests PRIVATE
        PYTHON3_EXECUTABLE="${PYTHON3_EXECUTABLE}"
        GEN_FW_META_SCRIPT="${REPO_ROOT}/tools/gen_fw_meta.py"
        TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}")
else()
    message(STATUS "python3 not found: gen_fw_meta_script will be reported as skipped")
    add_executable(gen_fw_meta_skip mocks/skip_test.c)
    add_test(NAME gen_fw_meta_script COMMAND gen_fw_meta_skip)
    set_tests_properties(gen_fw_meta_script PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---- T-19 / FR-25 host part: the updater page script in Node -----------------------------------------------------------
# dump_updater_page prints the page as updater_http.c assembles it. Without Node the test is registered as skipped
# (exit code 77), never silently passed (same pattern as test/tuner-read-measurement).
add_executable(dump_updater_page page_js/dump_updater_page.c ${UPDATER_DIR}/updater_page.c)
target_include_directories(dump_updater_page PRIVATE ${UPDATER_DIR} ${FW_INCLUDES})
target_compile_options(dump_updater_page PRIVATE -Wall -Wextra)
find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
    add_test(NAME updater_page_js
             COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/page_js/test_updater_page_js.js
                     $<TARGET_FILE:dump_updater_page>)
else()
    message(STATUS "Node.js not found: updater_page_js will be reported as skipped")
    add_test(NAME updater_page_js COMMAND dump_updater_page --skip)
endif()
set_tests_properties(updater_page_js PROPERTIES SKIP_RETURN_CODE 77)
