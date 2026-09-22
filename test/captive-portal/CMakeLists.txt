cmake_minimum_required(VERSION 3.16)
project(captive_portal_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ucontext (freertos_mock) needs the GNU dialect
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN_DIR ${REPO_ROOT}/main)
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_DIR ${REPO_ROOT}/third_party/fff)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)
include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)

# Headers of the components under test (no ESP-IDF headers are used; mocks/ replaces them).
set(COMPONENT_INCLUDES
    ${MAIN_DIR}/logging/include
    ${MAIN_DIR}/button/include
    ${MAIN_DIR}/credential_store/include
    ${MAIN_DIR}/wifi_manager/include
    ${MAIN_DIR}/dns_server/include
    ${MAIN_DIR}/http_portal/include
    ${MAIN_DIR}/provisioning/include
    ${MAIN_DIR}/ws2812_timing/include)

# Shared mocks: log capture + esp_err_to_name, fake FreeRTOS, in-memory NVS.
add_library(host_stubs STATIC ${MOCK_DIR}/host_stubs.c)
target_include_directories(host_stubs PUBLIC ${MOCK_DIR} ${COMPONENT_INCLUDES})

add_library(freertos_mock STATIC ${MOCK_DIR}/freertos_mock.c)
target_include_directories(freertos_mock PUBLIC ${MOCK_DIR})

add_library(nvs_fake STATIC ${MOCK_DIR}/nvs_fake.c)
target_include_directories(nvs_fake PUBLIC ${MOCK_DIR})

# Add one Catch2 test executable. Sources are the tests plus the component code under test.
function(add_feature_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${MOCK_DIR} ${FFF_DIR} ${COMPONENT_INCLUDES})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain host_stubs freertos_mock nvs_fake)
    catch_discover_tests(${name})
endfunction()

# T-2, T-7, T-12: encrypted credential store.
add_feature_test(credential_store_tests
    test_credential_store.cpp
    ${MAIN_DIR}/credential_store/credential_store.c)

# T-1: button timing. gpio_fakes.c holds the FFF globals for this binary.
add_feature_test(button_tests
    test_button.cpp
    ${MAIN_DIR}/button/button.c
    ${MOCK_DIR}/gpio_fakes.c)

# T-4: wildcard DNS responder.
add_feature_test(dns_server_tests
    test_dns_server.cpp
    ${MAIN_DIR}/dns_server/dns_server.c)

# T-3, T-5, T-10: Wi-Fi manager. The harness compiles wifi_manager.c so its state can be reset per case.
add_feature_test(wifi_manager_tests
    test_wifi_manager.cpp
    ${MOCK_DIR}/wifi_manager_harness.c
    ${MOCK_DIR}/wifi_fakes.c
    ${MAIN_DIR}/credential_store/credential_store.c)

# T-4..T-7: HTTP portal and its pure helpers. The harness compiles http_portal.c so its state can be reset per case.
# SPEC-003 (ws2812-tuner-page) added GET/POST /tuner to http_portal.c, which pulls in tuner_page.c
# (the embedded page) and ws2812_timing.c (parsing/validation/logging); both are linked in so the
# file still builds. See test/ws2812-tuner-page/ for the SPEC-003 tuner-specific test coverage.
add_feature_test(http_portal_tests
    test_http_portal.cpp
    test_portal_form.cpp
    ${MOCK_DIR}/http_portal_harness.c
    ${MOCK_DIR}/httpd_mock.c
    ${MAIN_DIR}/http_portal/portal_form.c
    ${MAIN_DIR}/http_portal/tuner_page.c
    ${MAIN_DIR}/ws2812_timing/ws2812_timing.c
    ${MAIN_DIR}/credential_store/credential_store.c)

# T-1, T-2, T-9, T-10: orchestrator state machine, driven by the fake clock with every component faked.
add_feature_test(provisioning_tests
    test_provisioning.cpp
    ${MOCK_DIR}/provisioning_harness.c
    ${MOCK_DIR}/provisioning_fakes.c)
