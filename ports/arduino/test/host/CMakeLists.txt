cmake_minimum_required(VERSION 3.16)

project(honch_arduino_host_tests C CXX)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../../core ${CMAKE_BINARY_DIR}/core)

add_executable(honch_arduino_wrapper_test
    test_arduino_wrapper.cpp
    ../../src/Honch.cpp
    ../../src/honch_arduino_platform.cpp
    ../../src/honch_arduino_storage.cpp
    ../../src/honch_arduino_transport.cpp
)

target_include_directories(honch_arduino_wrapper_test PRIVATE
    ../../src
    ../../../../core/src
)

target_link_libraries(honch_arduino_wrapper_test PRIVATE honch_core)

target_compile_features(honch_arduino_wrapper_test PRIVATE cxx_std_20)

target_compile_options(honch_arduino_wrapper_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
