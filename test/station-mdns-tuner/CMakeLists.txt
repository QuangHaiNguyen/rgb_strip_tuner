cmake_minimum_required(VERSION 3.16)
project(station_mdns_tuner_tests C CXX)

# Host tests for SPEC-005 (docs/specs/station-mdns-tuner.md): the station profile of http_portal, the Origin
# check, the station page, mdns_service, the got-IP reporting of wifi_manager and the station-services lifecycle
# of the provisioning orchestrator. Standalone CMake project, independent of the ESP-IDF build and of the managed
# espressif/mdns component (NFR-11): mocks/mdns.h replaces it.
#
# As SPEC-005 section 10 asks, the test/captive-portal mocks are reused rather than copied: the ESP-IDF header
# mocks, the httpd simulator, the fake FreeRTOS clock, the wifi_manager/http_portal/provisioning harnesses and the
# orchestrator's component fakes (which gained the SPEC-005 fakes). The FFF LogWrite() fake of
# test/ws2812-tuner-page is reused where a test needs FFF log capture next to non-FFF fakes.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ucontext (freertos_mock) needs the GNU dialect
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)
set(SHARED_MOCK_DIR ${REPO_ROOT}/test/captive-portal/mocks)
set(LOG_FAKE_DIR ${REPO_ROOT}/test/ws2812-tuner-page/mocks)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)

set(COMPONENT_INCLUDES
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/button/include
    ${MAIN_DIR}/credential_store/include
    ${MAIN_DIR}/wifi_manager/include
    ${MAIN_DIR}/dns_server/include
    ${MAIN_DIR}/http_portal/include
    ${MAIN_DIR}/provisioning/include
    ${MAIN_DIR}/ws2812_timing/include
    ${MAIN_DIR}/led_controller/include
    ${MAIN_DIR}/mdns_service/include
    # SPEC-004 FR-34 (2026-10-03): provisioning.c includes rmt_pulse_monitor.h (header only; the API is faked).
    ${MAIN_DIR}/rmt_pulse_monitor/include)

# MOCK_DIR (mdns.h) and SHARED_MOCK_DIR (ESP-IDF header mocks) come before any component header.
set(TEST_INCLUDES ${MOCK_DIR} ${SHARED_MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

add_library(freertos_mock STATIC ${SHARED_MOCK_DIR}/freertos_mock.c)
target_include_directories(freertos_mock PUBLIC ${SHARED_MOCK_DIR})

add_library(nvs_fake STATIC ${SHARED_MOCK_DIR}/nvs_fake.c)
target_include_directories(nvs_fake PUBLIC ${SHARED_MOCK_DIR})

# Hand-written LogWrite() capture (test/captive-portal), for binaries whose FFF globals live in another fake file.
add_library(host_stubs STATIC ${SHARED_MOCK_DIR}/host_stubs.c)
target_include_directories(host_stubs PUBLIC ${SHARED_MOCK_DIR} ${COMPONENT_INCLUDES})

# FFF LogWrite() fake (test/ws2812-tuner-page); it also holds DEFINE_FFF_GLOBALS for the binary that links it.
add_library(log_fakes STATIC ${LOG_FAKE_DIR}/log_fakes.c)
target_include_directories(log_fakes PUBLIC ${LOG_FAKE_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

# T-19 (FR-27, NFR-13): the pure Origin predicate against the 22 section 7.5 vectors.
add_feature_test(station_origin_tests
    test_station_origin.cpp
    ${MAIN_DIR}/http_portal/station_origin.c)

# T-5 (FR-15): static checks of g_tuner_page_station against g_tuner_page. tuner_page.h is private to http_portal.
add_feature_test(station_page_static_tests
    test_station_page_static.cpp
    ${MAIN_DIR}/http_portal/tuner_page.c)
target_include_directories(station_page_static_tests PRIVATE ${MAIN_DIR}/http_portal)

# T-3, T-4, T-20 (FR-7, FR-11..FR-14, FR-16, FR-27..FR-30): the station profile through the httpd simulator.
# The shared harness compiles http_portal.c so its statics can be reset; credential_store.c is linked for the
# provisioning /submit handler compiled in the same file (as in tuner_http_tests).
add_feature_test(station_http_tests
    test_station_http.cpp
    ${SHARED_MOCK_DIR}/http_portal_harness.c
    ${SHARED_MOCK_DIR}/httpd_mock.c
    ${LOG_FAKE_DIR}/esp_err_stub.c
    ${MAIN_DIR}/http_portal/portal_form.c
    ${MAIN_DIR}/http_portal/station_origin.c
    ${MAIN_DIR}/http_portal/tuner_page.c
    ${MAIN_DIR}/http_portal/tuner_result.c     # SPEC-003 FR-25/FR-26, SPEC-005 FR-31 (2026-10-03)
    ${MAIN_DIR}/ws2812_timing/ws2812_timing.c
    ${MAIN_DIR}/credential_store/credential_store.c)
target_include_directories(station_http_tests PRIVATE ${LOG_FAKE_DIR} ${MAIN_DIR}/http_portal)
target_link_libraries(station_http_tests PRIVATE log_fakes freertos_mock nvs_fake)

# T-6 (FR-17..FR-21): mdns_service.c against the FFF fakes of the mDNS API.
add_feature_test(mdns_service_tests
    test_mdns_service.cpp
    ${MOCK_DIR}/mdns_service_harness.c
    ${MOCK_DIR}/mdns_fakes.c)
target_include_directories(mdns_service_tests PRIVATE ${LOG_FAKE_DIR})
target_link_libraries(mdns_service_tests PRIVATE log_fakes)

# T-1, wifi_manager part (FR-1, FR-2, FR-29 GetWifiStationAddress()).
add_feature_test(station_wifi_tests
    test_station_wifi.cpp
    ${SHARED_MOCK_DIR}/wifi_manager_harness.c
    ${SHARED_MOCK_DIR}/wifi_fakes.c
    ${MAIN_DIR}/credential_store/credential_store.c)
target_link_libraries(station_wifi_tests PRIVATE host_stubs freertos_mock nvs_fake)

# T-1 (orchestrator mapping), T-2 (FR-3..FR-10, FR-29, NFR-4, NFR-13) and the host part of NFR-5: the orchestrator
# on the fake clock with every component faked (test/captive-portal/mocks/provisioning_fakes.c).
add_feature_test(station_lifecycle_tests
    test_station_lifecycle.cpp
    ${SHARED_MOCK_DIR}/provisioning_harness.c
    ${SHARED_MOCK_DIR}/provisioning_fakes.c)
target_link_libraries(station_lifecycle_tests PRIVATE host_stubs freertos_mock)

# T-7 (static review), mechanical part: source scans of main/ and the manifest/lock files (REPO_ROOT at test time).
add_feature_test(station_static_review_tests
    test_static_review.cpp)
target_compile_definitions(station_static_review_tests PRIVATE REPO_ROOT="${REPO_ROOT}")

# SPEC-005 FR-15 / SPEC-003 T-21 host part (2026-10-03): the station page's script (shared fragments) run in Node by the
# harness of test/ws2812-tuner-page/page_js. Without Node the test is reported as skipped (exit code 77).
set(PAGE_JS_DIR ${REPO_ROOT}/test/ws2812-tuner-page/page_js)
add_executable(dump_tuner_page ${PAGE_JS_DIR}/dump_tuner_page.c ${MAIN_DIR}/http_portal/tuner_page.c)
target_include_directories(dump_tuner_page PRIVATE ${MAIN_DIR}/http_portal)
target_compile_options(dump_tuner_page PRIVATE -Wall -Wextra)
find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
    add_test(NAME tuner_page_js_station
             COMMAND ${NODE_EXECUTABLE} ${PAGE_JS_DIR}/test_tuner_page_js.js $<TARGET_FILE:dump_tuner_page> station)
else()
    message(STATUS "Node.js not found: tuner_page_js_station will be reported as skipped")
    add_test(NAME tuner_page_js_station COMMAND dump_tuner_page --skip)
endif()
set_tests_properties(tuner_page_js_station PROPERTIES SKIP_RETURN_CODE 77)
