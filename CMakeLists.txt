cmake_minimum_required(VERSION 3.16)

if(ESP_PLATFORM)
    include(${CMAKE_CURRENT_LIST_DIR}/ports/esp-idf/honch/CMakeLists.txt)
else()
    project(honch_sdk C)

    option(HONCH_BUILD_TESTS "Build Honch SDK tests" OFF)
    option(HONCH_BUILD_BENCHMARKS "Build Honch SDK benchmarks" OFF)
    option(HONCH_BUILD_ARDUINO_HOST_TESTS "Build Arduino host wrapper tests" OFF)

    if(HONCH_BUILD_TESTS)
        enable_testing()
    endif()

    add_subdirectory(core)

    if(HONCH_BUILD_ARDUINO_HOST_TESTS)
        enable_language(CXX)
        add_subdirectory(ports/arduino/test/host)
    endif()
endif()
