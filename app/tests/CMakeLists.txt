# SPDX-License-Identifier: MIT

# Inside BitBake, GoogleTest comes from the googletest recipe (meta-oe) via
# DEPENDS and the build must not touch the network, so FetchContent is only
# used for host builds where GoogleTest is not installed.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    if(NOT TELEMETRY_FETCH_GTEST)
        message(FATAL_ERROR "GoogleTest not found and TELEMETRY_FETCH_GTEST is OFF")
    endif()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723  # v1.17.0, same version as meta-oe
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(telemetry_tests
    test_config.cpp
    test_health_monitor.cpp
    test_line_buffer.cpp
    test_packet.cpp
    test_status_json.cpp)
target_link_libraries(telemetry_tests PRIVATE telemetry_core telemetry_options GTest::gtest_main)

include(GoogleTest)
# PRE_TEST lists the tests when ctest runs rather than right after linking,
# so cross-compiled builds (which cannot execute the binary) still work.
gtest_discover_tests(telemetry_tests DISCOVERY_MODE PRE_TEST)

if(NOT CMAKE_CROSSCOMPILING)
    # End-to-end: real daemon + simulator over localhost UDP.
    add_test(NAME integration
             COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.sh
                     $<TARGET_FILE:telemetryd> $<TARGET_FILE:telemetry-sim>)
    set_tests_properties(integration PROPERTIES TIMEOUT 60)

    add_test(NAME example_config_is_valid
             COMMAND telemetryd --check-config --config ${PROJECT_SOURCE_DIR}/config/telemetryd.conf.example)
endif()
