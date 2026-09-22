cmake_minimum_required(VERSION 3.16)
project(ws2812_tuner_page_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ucontext (freertos_mock, reused from test/captive-portal) needs the GNU dialect
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)
# http_portal.c, its esp_http_server.h host simulator and the FreeRTOS mock already exist for
# SPEC-002 under test/captive-portal/mocks; SPEC-003 reuses them (esp_http_server.h, httpd_mock.*,
# freertos_mock.*) instead of duplicating a second copy of the same simulator.
set(SHARED_MOCK_DIR ${REPO_ROOT}/test/captive-portal/mocks)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)

# Headers of the components under test (no ESP-IDF headers are used by ws2812_timing; mocks/ and
# SHARED_MOCK_DIR replace esp_http_server.h/FreeRTOS for http_portal.c).
set(COMPONENT_INCLUDES
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/ws2812_timing/include
    ${MAIN_DIR}/http_portal/include
    ${MAIN_DIR}/credential_store/include
    ${MAIN_DIR}/wifi_manager/include)

# FFF fake of LogWrite() (T-2, NFR-15), linked into every executable below.
add_library(log_fakes STATIC ${MOCK_DIR}/log_fakes.c)
target_include_directories(log_fakes PUBLIC ${MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})

# Reused from test/captive-portal (see SHARED_MOCK_DIR note above).
add_library(freertos_mock STATIC ${SHARED_MOCK_DIR}/freertos_mock.c)
target_include_directories(freertos_mock PUBLIC ${SHARED_MOCK_DIR})

add_library(nvs_fake STATIC ${SHARED_MOCK_DIR}/nvs_fake.c)
target_include_directories(nvs_fake PUBLIC ${SHARED_MOCK_DIR})

# Add one Catch2 test executable. Sources are the tests plus the component code under test.
function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${MOCK_DIR} ${SHARED_MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain log_fakes)
    catch_discover_tests(${name})
endfunction()

# T-1, T-2, T-3: ParseTunerForm(), ValidateWs2812Timing(), LogWs2812Timing()/LogWs2812Rejection().
# Pure logic, no ESP-IDF dependency other than logging.h (NFR-15).
add_feature_test(ws2812_timing_tests
    test_ws2812_timing.cpp
    test_ws2812_logging.cpp
    ${MAIN_DIR}/ws2812_timing/ws2812_timing.c)

# T-4, T-12: the GET/POST /tuner handlers through the http_portal simulator, to check side effects
# and hostile-input handling end to end. TUNER_SRC/HTTP_PORTAL_SRC feed the T-4 grep-style static
# check (section 10: "grep confirms no such includes/calls in the new code").
# portal_form.c's ValidateSubmission() (used by the unrelated /submit handler compiled in the same
# translation unit) calls IsCredentialsValid(), so credential_store.c is linked in too, exactly as
# it is for SPEC-002's http_portal_tests; the tuner tests never exercise that code path.
add_feature_test(tuner_http_tests
    test_tuner_http.cpp
    ${MOCK_DIR}/http_portal_harness.c
    ${MOCK_DIR}/esp_err_stub.c
    ${SHARED_MOCK_DIR}/httpd_mock.c
    ${MAIN_DIR}/http_portal/portal_form.c
    ${MAIN_DIR}/http_portal/tuner_page.c
    ${MAIN_DIR}/ws2812_timing/ws2812_timing.c
    ${MAIN_DIR}/credential_store/credential_store.c)
target_link_libraries(tuner_http_tests PRIVATE freertos_mock nvs_fake)
target_compile_definitions(tuner_http_tests PRIVATE
    WS2812_TIMING_SRC="${MAIN_DIR}/ws2812_timing/ws2812_timing.c"
    HTTP_PORTAL_SRC="${MAIN_DIR}/http_portal/http_portal.c")
