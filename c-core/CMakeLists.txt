cmake_minimum_required(VERSION 3.20)

project(honch_c_core C)

option(HONCH_BUILD_TESTS "Build Honch C/POSIX SDK tests" ON)
option(HONCH_BUILD_EXAMPLES "Build Honch C/POSIX SDK examples" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(HONCH_BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)

add_library(honch_c_core
    src/honch.c
    src/honch_json.c
    src/honch_platform.c
    src/honch_queue.c
    src/honch_state.c
    src/honch_transport_curl.c
)

target_include_directories(honch_c_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(honch_c_core
    PRIVATE
        CURL::libcurl
)

target_compile_options(honch_c_core PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_TESTS)
    target_compile_definitions(honch_c_core PRIVATE HONCH_TESTING)

    add_executable(honch_c_core_tests
        tests/test_honch.c
    )

    target_link_libraries(honch_c_core_tests PRIVATE honch_c_core)
    target_compile_definitions(honch_c_core_tests PRIVATE HONCH_TESTING)
    target_compile_options(honch_c_core_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_c_core_tests COMMAND honch_c_core_tests)
endif()

if(HONCH_BUILD_EXAMPLES)
    add_subdirectory(examples/posix_device)
    add_subdirectory(examples/connected_camera)
endif()
