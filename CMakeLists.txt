cmake_minimum_required(VERSION 3.16)

project(miniredis
    VERSION 1.0.0
    DESCRIPTION "A Redis-style in-memory database built from scratch in C++20"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIREDIS_BUILD_TESTS "Build the MiniRedis test suite" ON)
option(MINIREDIS_BUILD_BENCHMARK "Build the MiniRedis benchmark tool" ON)
option(MINIREDIS_WERROR "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Core library: everything except the three entry points.
# ---------------------------------------------------------------------------

file(GLOB MINIREDIS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER MINIREDIS_CORE_SOURCES EXCLUDE REGEX "src/main_.*\\.cpp$")

add_library(miniredis_core STATIC ${MINIREDIS_CORE_SOURCES})
target_include_directories(miniredis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(miniredis_core PUBLIC /W4 /permissive- /Zc:preprocessor)
    target_compile_definitions(miniredis_core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MINIREDIS_WERROR)
        target_compile_options(miniredis_core PUBLIC /WX)
    endif()
else()
    target_compile_options(miniredis_core PUBLIC -Wall -Wextra -Wno-unknown-pragmas)
    if(MINIREDIS_WERROR)
        target_compile_options(miniredis_core PUBLIC -Werror)
    endif()
endif()

# Platform libraries: Winsock on Windows, pthreads elsewhere.
if(WIN32)
    target_link_libraries(miniredis_core PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(miniredis_core PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

add_executable(miniredis-server ${CMAKE_CURRENT_SOURCE_DIR}/src/main_server.cpp)
target_link_libraries(miniredis-server PRIVATE miniredis_core)

add_executable(miniredis-cli ${CMAKE_CURRENT_SOURCE_DIR}/src/main_cli.cpp)
target_link_libraries(miniredis-cli PRIVATE miniredis_core)

if(MINIREDIS_BUILD_BENCHMARK)
    add_executable(miniredis-benchmark ${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark.cpp)
    target_link_libraries(miniredis-benchmark PRIVATE miniredis_core)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(MINIREDIS_BUILD_TESTS)
    enable_testing()
    file(GLOB MINIREDIS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    if(MINIREDIS_TEST_SOURCES)
        add_executable(miniredis-tests ${MINIREDIS_TEST_SOURCES})
        target_link_libraries(miniredis-tests PRIVATE miniredis_core)
        target_include_directories(miniredis-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_test(NAME unit COMMAND miniredis-tests)
    endif()
endif()

message(STATUS "MiniRedis ${PROJECT_VERSION} - build type: ${CMAKE_BUILD_TYPE}")
