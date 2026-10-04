cmake_minimum_required(VERSION 3.16)
project(tuner_read_measurement_tests C CXX)

# Host tests for SPEC-006 (docs/specs/tuner-read-measurement.md, "Tuner Read button"): AnalyzeWs2812Read(), the read
# result body, POST /tuner/read in both server profiles, the orchestrator hand-off and the read capture mode of
# rmt_pulse_monitor, plus the host-checkable parts of the page (T-1 to T-6). Standalone CMake project, independent of
# the ESP-IDF build.
#
# As in the other suites, existing mocks are reused rather than copied:
# - test/led-controller/mocks: RMT/FreeRTOS FFF fakes, driver/freertos header replacements and the pulse-monitor
#   harness (extended in mocks/rmt_pulse_monitor_read_harness.c for the SPEC-006 capture-mode statics);
# - test/captive-portal/mocks: httpd simulator, fake FreeRTOS clock, NVS fake, http_portal and provisioning harnesses,
#   orchestrator component fakes (with the ArmPulseRead() fake);
# - test/ws2812-tuner-page/mocks: FFF LogWrite() fake (also holds the FFF globals of the binaries that link it) and
#   page_js/dump_tuner_page.c for the Node page-script test.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ucontext (freertos_mock) and anonymous unions need the GNU dialect
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)
set(LED_MOCK_DIR ${REPO_ROOT}/test/led-controller/mocks)
set(PORTAL_MOCK_DIR ${REPO_ROOT}/test/captive-portal/mocks)
set(LOG_FAKE_DIR ${REPO_ROOT}/test/ws2812-tuner-page/mocks)
set(PAGE_JS_DIR ${REPO_ROOT}/test/ws2812-tuner-page/page_js)

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
    ${MAIN_DIR}/rmt_pulse_monitor/include)

function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

# ---- rmt_pulse_monitor (T-1, T-5): led-controller RMT/FreeRTOS fakes, FFF LogWrite() fake ---------------------------
# LED_MOCK_DIR comes first: its driver/rmt_*.h, freertos/*.h and esp_err.h replace the ESP-IDF headers.
set(PULSE_INCLUDES ${MOCK_DIR} ${LED_MOCK_DIR} ${LOG_FAKE_DIR} ${FFF_DIR}
    ${MAIN_DIR}/logging/include ${MAIN_DIR}/ws2812_timing/include ${MAIN_DIR}/rmt_pulse_monitor/include)
add_library(read_pulse_fakes STATIC
    ${LOG_FAKE_DIR}/log_fakes.c
    ${LED_MOCK_DIR}/freertos_fakes.c
    ${LED_MOCK_DIR}/rmt_fakes.c)
target_include_directories(read_pulse_fakes PUBLIC ${PULSE_INCLUDES})

# T-1 (FR-15, NFR-8): AnalyzeWs2812Read() against vectors R1 to R11. rmt_pulse_monitor.c is compiled as-is; the RMT and
# FreeRTOS fakes only satisfy the linker.
add_feature_test(read_analysis_tests
    test_read_analysis.cpp
    ${MAIN_DIR}/rmt_pulse_monitor/rmt_pulse_monitor.c)
target_include_directories(read_analysis_tests PRIVATE ${PULSE_INCLUDES})
target_link_libraries(read_analysis_tests PRIVATE read_pulse_fakes)
target_compile_definitions(read_analysis_tests PRIVATE
    RMT_PULSE_MONITOR_SRC="${MAIN_DIR}/rmt_pulse_monitor/rmt_pulse_monitor.c")

# T-5 (FR-11 to FR-14, FR-16, FR-17, NFR-3, NFR-4): ArmPulseRead() and the decode task's read mode, through the
# led-controller pulse-monitor harness plus the SPEC-006 extension in mocks/.
add_feature_test(read_pulse_monitor_tests
    test_read_pulse_monitor.cpp
    ${MOCK_DIR}/rmt_pulse_monitor_read_harness.c)
target_include_directories(read_pulse_monitor_tests PRIVATE ${PULSE_INCLUDES})
target_link_libraries(read_pulse_monitor_tests PRIVATE read_pulse_fakes)
target_compile_definitions(read_pulse_monitor_tests PRIVATE
    RMT_PULSE_MONITOR_SRC="${MAIN_DIR}/rmt_pulse_monitor/rmt_pulse_monitor.c")

# T-5 / T-8 host part (FR-13, use case 8.3): the 2026-10-04 overlap-race fix (s_is_receive_pending) through the same
# harness: a second arm while a receive is in flight, the done ISR racing a failed arm, the flag never getting stuck.
add_feature_test(read_overlap_tests
    test_read_overlap.cpp
    ${MOCK_DIR}/rmt_pulse_monitor_read_harness.c)
target_include_directories(read_overlap_tests PRIVATE ${PULSE_INCLUDES})
target_link_libraries(read_overlap_tests PRIVATE read_pulse_fakes)

# ---- http_portal (T-2, T-3, T-6 static) --------------------------------------------------------------------------------
# T-2 (FR-18 to FR-20, NFR-8): GetTunerResultState() / FormatTunerResult(); tuner_result.c has no ESP-IDF dependency.
add_feature_test(read_result_tests
    test_read_result.cpp
    ${MAIN_DIR}/http_portal/tuner_result.c)
target_include_directories(read_result_tests PRIVATE ${COMPONENT_INCLUDES})

# T-3 (FR-7, FR-8, NFR-10) and the http_portal part of T-4 (FR-9): POST /tuner/read in both profiles through the httpd
# simulator. The captive-portal harness compiles http_portal.c so its statics can be reset per case.
add_library(read_freertos_mock STATIC ${PORTAL_MOCK_DIR}/freertos_mock.c)
target_include_directories(read_freertos_mock PUBLIC ${PORTAL_MOCK_DIR})
add_library(read_nvs_fake STATIC ${PORTAL_MOCK_DIR}/nvs_fake.c)
target_include_directories(read_nvs_fake PUBLIC ${PORTAL_MOCK_DIR})
add_library(read_log_fakes STATIC ${LOG_FAKE_DIR}/log_fakes.c)
target_include_directories(read_log_fakes PUBLIC ${LOG_FAKE_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

add_feature_test(read_http_tests
    test_read_http.cpp
    ${PORTAL_MOCK_DIR}/http_portal_harness.c
    ${PORTAL_MOCK_DIR}/httpd_mock.c
    ${LOG_FAKE_DIR}/esp_err_stub.c
    ${MAIN_DIR}/http_portal/portal_form.c
    ${MAIN_DIR}/http_portal/station_origin.c
    ${MAIN_DIR}/http_portal/tuner_page.c
    ${MAIN_DIR}/http_portal/tuner_result.c
    ${MAIN_DIR}/ws2812_timing/ws2812_timing.c
    ${MAIN_DIR}/credential_store/credential_store.c)
target_include_directories(read_http_tests PRIVATE ${PORTAL_MOCK_DIR} ${LOG_FAKE_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})
target_link_libraries(read_http_tests PRIVATE read_log_fakes read_freertos_mock read_nvs_fake)
target_compile_definitions(read_http_tests PRIVATE HTTP_PORTAL_SRC="${MAIN_DIR}/http_portal/http_portal.c")

# T-6 static part (FR-1, FR-5, NFR-1): the embedded pages. tuner_page.h is private to http_portal.
add_feature_test(read_page_static_tests
    test_read_page_static.cpp
    ${MAIN_DIR}/http_portal/tuner_page.c)
target_include_directories(read_page_static_tests PRIVATE ${MAIN_DIR}/http_portal ${COMPONENT_INCLUDES})

# ---- provisioning (T-4: FR-9, FR-10) -------------------------------------------------------------------------------
# The orchestrator on the fake clock with every component faked (captive-portal provisioning_fakes.c, which holds the
# FFF globals of this binary and the ArmPulseRead() fake); host_stubs.c captures the log.
add_library(read_host_stubs STATIC ${PORTAL_MOCK_DIR}/host_stubs.c)
target_include_directories(read_host_stubs PUBLIC ${PORTAL_MOCK_DIR} ${COMPONENT_INCLUDES})
add_feature_test(read_orchestrator_tests
    test_read_orchestrator.cpp
    ${PORTAL_MOCK_DIR}/provisioning_harness.c
    ${PORTAL_MOCK_DIR}/provisioning_fakes.c)
target_include_directories(read_orchestrator_tests PRIVATE ${PORTAL_MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})
target_link_libraries(read_orchestrator_tests PRIVATE read_host_stubs read_freertos_mock)

# ---- Page script in Node (T-6 host part: FR-2 to FR-6, FR-20) -------------------------------------------------------
# dump_tuner_page (test/ws2812-tuner-page/page_js) prints each page exactly as tuner_page.c builds it. Without Node the
# tests are registered as skipped (exit code 77), never silently passed.
add_executable(read_dump_tuner_page ${PAGE_JS_DIR}/dump_tuner_page.c ${MAIN_DIR}/http_portal/tuner_page.c)
target_include_directories(read_dump_tuner_page PRIVATE ${MAIN_DIR}/http_portal)
target_compile_options(read_dump_tuner_page PRIVATE -Wall -Wextra)
find_program(NODE_EXECUTABLE NAMES node nodejs)
foreach(page provisioning station)
    if(NODE_EXECUTABLE)
        add_test(NAME read_page_js_${page}
                 COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/page_js/test_read_page_js.js
                         $<TARGET_FILE:read_dump_tuner_page> ${page})
    else()
        add_test(NAME read_page_js_${page} COMMAND read_dump_tuner_page --skip)
    endif()
    set_tests_properties(read_page_js_${page} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
if(NOT NODE_EXECUTABLE)
    message(STATUS "Node.js not found: read_page_js_* will be reported as skipped")
endif()
