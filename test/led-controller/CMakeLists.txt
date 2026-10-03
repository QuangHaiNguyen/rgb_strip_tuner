cmake_minimum_required(VERSION 3.16)
project(led_controller_tests C CXX)

# Host tests for SPEC-004 (docs/specs/led-controller.md): main/led_controller/ and
# main/rmt_pulse_monitor/. Standalone CMake project, independent of the ESP-IDF build.
# The http_portal -> provisioning hand-off (T-4, T-5) extends the existing SPEC-003/SPEC-002
# suites, as the spec's test plan asks: see test/ws2812-tuner-page/test_tuner_http.cpp and
# test/captive-portal/test_provisioning.cpp.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # anonymous union members and `= {}` initializers used by the IDF-style code
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)
# The FFF LogWrite() fake (with captured text and per-level counts) already exists for SPEC-003;
# it is reused here rather than copied. Its log_fakes.c also holds the FFF globals of every binary below.
set(LOG_FAKE_DIR ${REPO_ROOT}/test/ws2812-tuner-page/mocks)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)

# MOCK_DIR comes first: its driver/rmt_*.h, freertos/*.h and esp_err.h replace the ESP-IDF headers.
set(TEST_INCLUDES
    ${MOCK_DIR}
    ${LOG_FAKE_DIR}
    ${FFF_DIR}
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/ws2812_timing/include
    ${MAIN_DIR}/led_controller/include
    ${MAIN_DIR}/rmt_pulse_monitor/include)

# FFF fakes shared by every executable: LogWrite (+ FFF globals), FreeRTOS, and the RMT driver.
add_library(led_test_fakes STATIC
    ${LOG_FAKE_DIR}/log_fakes.c
    ${MOCK_DIR}/freertos_fakes.c
    ${MOCK_DIR}/rmt_fakes.c)
target_include_directories(led_test_fakes PUBLIC ${TEST_INCLUDES})

function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${TEST_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain led_test_fakes)
    catch_discover_tests(${name})
endfunction()

# T-1, T-2, T-12, T-13, T-14: the pure functions of both components (NFR-10, NFR-14).
# Both sources are compiled as-is; the RMT/FreeRTOS fakes only satisfy the linker.
add_feature_test(led_pure_tests
    test_led_pure.cpp
    ${MAIN_DIR}/led_controller/led_controller.c
    ${MAIN_DIR}/rmt_pulse_monitor/rmt_pulse_monitor.c)

# T-3, T-6, T-7 and the FR-1/FR-2/FR-13/FR-16..FR-18/FR-24 driver behavior. The harness compiles
# led_controller.c so its statics are reachable; ArmPulseCapture() is an FFF fake.
add_feature_test(led_controller_tests
    test_led_controller.cpp
    ${MOCK_DIR}/led_controller_harness.c
    ${MOCK_DIR}/pulse_monitor_fakes.c)
target_compile_definitions(led_controller_tests PRIVATE
    LED_CONTROLLER_SRC="${MAIN_DIR}/led_controller/led_controller.c")

# FR-19..FR-25, FR-29..FR-32: StartPulseMonitor(), ArmPulseCapture(), the ISR callback and the
# decode task loop. The harness compiles rmt_pulse_monitor.c so its statics are reachable.
add_feature_test(pulse_monitor_tests
    test_rmt_pulse_monitor.cpp
    ${MOCK_DIR}/rmt_pulse_monitor_harness.c)
target_compile_definitions(pulse_monitor_tests PRIVATE
    RMT_PULSE_MONITOR_SRC="${MAIN_DIR}/rmt_pulse_monitor/rmt_pulse_monitor.c")

# SPEC-004 T-19/T-20 across components (2026-10-03): led_controller.c and the real rmt_pulse_monitor.c together, so
# that "frames skipped before arming publish nothing" (FR-16, FR-36 owner decision) is checked end to end.
add_feature_test(led_publish_integration_tests
    test_led_publish_integration.cpp
    ${MOCK_DIR}/led_controller_harness.c
    ${MOCK_DIR}/rmt_pulse_monitor_harness.c)
