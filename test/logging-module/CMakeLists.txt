cmake_minimum_required(VERSION 3.16)
project(logging_module_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LOGGING_SRC_DIR ${REPO_ROOT}/main/logging)
set(MOCK_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mocks)
set(FFF_INCLUDE_DIR ${REPO_ROOT}/third_party/fff)

add_subdirectory(${REPO_ROOT}/third_party/catch2 ${CMAKE_CURRENT_BINARY_DIR}/catch2)

# Real (pthread-backed) FreeRTOS queue/task semantics used by all test binaries.
add_library(freertos_mock STATIC mocks/fff_freertos.c)
target_include_directories(freertos_mock PUBLIC ${MOCK_INCLUDE_DIR})
target_link_libraries(freertos_mock PUBLIC Threads::Threads)

# FFF fakes for the hardware-facing calls (UART, timer).
add_library(hal_fakes STATIC mocks/fakes.c)
target_include_directories(hal_fakes PUBLIC ${MOCK_INCLUDE_DIR} ${FFF_INCLUDE_DIR})
target_link_libraries(hal_fakes PUBLIC freertos_mock)

function(add_logging_test_executable name)
    add_executable(${name} ${ARGN} ${LOGGING_SRC_DIR}/logging.c)
    target_include_directories(${name} PRIVATE
        ${MOCK_INCLUDE_DIR}
        ${FFF_INCLUDE_DIR}
        ${LOGGING_SRC_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_definitions(${name} PRIVATE
        LOGGING_SOURCE_FILE="${LOGGING_SRC_DIR}/logging.c"
    )
    target_link_libraries(${name} PRIVATE
        Catch2::Catch2WithMain
        hal_fakes
        freertos_mock
        Threads::Threads
    )
endfunction()

add_logging_test_executable(logging_tests
    test_logging.cpp
    test_module_debug.c
    test_module_warning.c
)

add_logging_test_executable(logging_tests_no_timestamp
    test_logging_no_timestamp.cpp
    test_module_no_timestamp.c
)
target_compile_definitions(logging_tests_no_timestamp PRIVATE CONFIG_LOG_TIMESTAMP_ENABLE=0)

add_logging_test_executable(logging_tests_preinit
    test_logging_preinit.cpp
    test_module_preinit.c
)

include(CTest)
include(${REPO_ROOT}/third_party/catch2/extras/Catch.cmake)
catch_discover_tests(logging_tests)
catch_discover_tests(logging_tests_no_timestamp)
catch_discover_tests(logging_tests_preinit)
