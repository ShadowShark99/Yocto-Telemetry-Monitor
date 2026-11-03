# SPDX-License-Identifier: MIT
#
# Builds the same way on a developer host and inside BitBake. Yocto's cmake
# class passes a toolchain file plus the target's sysroot, CFLAGS and install
# directories (CMAKE_INSTALL_PREFIX=/usr, ...), so nothing here may hard-code
# host paths or download anything when TELEMETRY_FETCH_GTEST is OFF.
cmake_minimum_required(VERSION 3.20)

project(telemetry-monitor
    VERSION 1.0.0
    DESCRIPTION "Embedded Linux telemetry monitor"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TELEMETRY_BUILD_TESTS "Build the GoogleTest unit tests" OFF)
option(TELEMETRY_FETCH_GTEST "Download GoogleTest if it is not installed (host builds only)" ON)
option(TELEMETRY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TELEMETRY_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_CROSSCOMPILING)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Shared compiler settings
# ---------------------------------------------------------------------------
add_library(telemetry_options INTERFACE)
target_compile_options(telemetry_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wformat=2 -Wundef -Wnon-virtual-dtor)
if(TELEMETRY_WARNINGS_AS_ERRORS)
    target_compile_options(telemetry_options INTERFACE -Werror)
endif()
if(TELEMETRY_SANITIZE)
    target_compile_options(telemetry_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(telemetry_options INTERFACE -fsanitize=address,undefined)
endif()

configure_file(include/telemetry/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/telemetry/version.hpp @ONLY)

# ---------------------------------------------------------------------------
# telemetry_core: portable logic (no Linux APIs) - unit tested
# ---------------------------------------------------------------------------
add_library(telemetry_core STATIC
    src/core/config.cpp
    src/core/health_monitor.cpp
    src/core/line_buffer.cpp
    src/core/packet.cpp
    src/core/status_json.cpp)
target_include_directories(telemetry_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
# PRIVATE: warnings apply to our code only; for a static library CMake still
# propagates the link options (sanitizers) to everything that links it.
target_link_libraries(telemetry_core PRIVATE telemetry_options)

# ---------------------------------------------------------------------------
# telemetry_platform: thin Linux layer (sockets, termios, systemd protocol)
# ---------------------------------------------------------------------------
add_library(telemetry_platform STATIC
    src/platform/log.cpp
    src/platform/sd_notify.cpp
    src/platform/serial_port.cpp
    src/platform/serial_source.cpp
    src/platform/udp_source.cpp)
target_include_directories(telemetry_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(telemetry_platform PUBLIC telemetry_core PRIVATE telemetry_options)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(telemetryd
    src/daemon/daemon.cpp
    src/daemon/main.cpp)
target_link_libraries(telemetryd PRIVATE telemetry_platform telemetry_options)

add_executable(telemetry-sim src/sim/main.cpp)
target_link_libraries(telemetry-sim PRIVATE telemetry_platform telemetry_options)

install(TARGETS telemetryd telemetry-sim RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(TELEMETRY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
