cmake_minimum_required(VERSION 3.20)

project(honch_c_core C)

option(HONCH_BUILD_TESTS "Build Honch C/POSIX SDK tests" ON)
option(HONCH_BUILD_EXAMPLES "Build Honch C/POSIX SDK examples" ON)
option(HONCH_BUILD_E2E "Build Honch C/POSIX SDK real ingest E2E test" OFF)
option(HONCH_BUILD_BENCHMARKS "Build Honch C/POSIX SDK benchmarks" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(HONCH_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB QUIET)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix ${CMAKE_CURRENT_BINARY_DIR}/ports/posix)

if(HONCH_BUILD_BENCHMARKS)
    add_library(honch_bench_alloc_support
        bench/honch_bench_alloc.c
    )
    target_include_directories(honch_bench_alloc_support
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/bench
    )
    target_compile_options(honch_bench_alloc_support PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

add_library(honch_c_core INTERFACE)

target_include_directories(honch_c_core
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/honch/include
)

target_link_libraries(honch_c_core
    INTERFACE
        honch_posix
)

if(HONCH_BUILD_BENCHMARKS)
    target_link_libraries(honch_c_core
        INTERFACE
            honch_bench_alloc_support
    )
endif()

if(HONCH_BUILD_TESTS)
    add_executable(honch_c_core_tests
        test/test_honch.c
    )

    target_link_libraries(honch_c_core_tests PRIVATE honch_c_core)
    target_compile_definitions(honch_c_core_tests PRIVATE
        HONCH_TESTING
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_c_core_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_c_core_tests COMMAND honch_c_core_tests)
    add_test(
        NAME honch_c_core_cbor_migration_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_cbor_migration.py
    )

    if(HONCH_BUILD_E2E)
        add_executable(honch_c_core_e2e
            test/test_e2e_capture.c
        )

        target_link_libraries(honch_c_core_e2e PRIVATE honch_c_core CURL::libcurl)
        target_compile_definitions(honch_c_core_e2e PRIVATE
            _POSIX_C_SOURCE=200809L
            _DEFAULT_SOURCE
            _DARWIN_C_SOURCE
        )
        target_compile_options(honch_c_core_e2e PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
        )

        add_test(NAME honch_c_core_e2e COMMAND honch_c_core_e2e)
        set_tests_properties(honch_c_core_e2e PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

if(HONCH_BUILD_EXAMPLES)
    add_subdirectory(example/posix_device)
    add_subdirectory(example/connected_camera)
    add_subdirectory(example/posix_gpio)
endif()

if(HONCH_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
