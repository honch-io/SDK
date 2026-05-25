cmake_minimum_required(VERSION 3.20)

project(honch_posix_sdk C)

option(HONCH_BUILD_TESTS "Build Honch POSIX SDK tests" ON)
option(HONCH_BUILD_EXAMPLES "Build Honch POSIX SDK examples" ON)
option(HONCH_BUILD_E2E "Build Honch POSIX SDK real ingest E2E test" OFF)
option(HONCH_BUILD_BENCHMARKS "Build Honch POSIX SDK benchmarks" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(HONCH_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

if(NOT TARGET honch_core)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

if(HONCH_BUILD_BENCHMARKS AND NOT TARGET honch_bench_alloc_support)
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

add_library(honch_posix
    src/posix_compat.c
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
    $<TARGET_OBJECTS:honch_core_objects>
)

target_include_directories(honch_posix
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
)

target_link_libraries(honch_posix
    PUBLIC
        honch_core_objects
    PRIVATE
        CURL::libcurl
        Threads::Threads
)

target_compile_definitions(honch_posix PRIVATE
    _POSIX_C_SOURCE=200809L
)

if(HONCH_BUILD_TESTS OR HONCH_BUILD_BENCHMARKS)
    target_compile_definitions(honch_posix PRIVATE HONCH_TESTING)
endif()

if(HONCH_BUILD_BENCHMARKS)
    target_compile_options(honch_posix PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/bench/honch_bench_alloc.h
    )
    target_link_libraries(honch_posix PRIVATE honch_bench_alloc_support)
endif()

target_compile_options(honch_posix PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_TESTS)
    add_executable(honch_posix_tests
        test/test_honch.c
    )

    target_link_libraries(honch_posix_tests PRIVATE honch_posix)
    target_compile_definitions(honch_posix_tests PRIVATE
        HONCH_TESTING
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_posix_tests COMMAND honch_posix_tests)

    add_executable(honch_posix_state_consistency_tests
        ../../core/test/test_state_consistency.c
    )

    target_link_libraries(honch_posix_state_consistency_tests PRIVATE honch_posix)
    target_include_directories(honch_posix_state_consistency_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )
    target_compile_definitions(honch_posix_state_consistency_tests PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_state_consistency_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_posix_state_consistency_tests COMMAND honch_posix_state_consistency_tests)

    add_test(
        NAME honch_posix_cbor_migration_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_cbor_migration.py
    )

    if(HONCH_BUILD_E2E)
        add_executable(honch_posix_e2e
            test/test_e2e_capture.c
        )

        target_link_libraries(honch_posix_e2e PRIVATE honch_posix CURL::libcurl)
        target_compile_definitions(honch_posix_e2e PRIVATE
            _POSIX_C_SOURCE=200809L
            _DEFAULT_SOURCE
            _DARWIN_C_SOURCE
        )
        target_compile_options(honch_posix_e2e PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
        )

        add_test(NAME honch_posix_e2e COMMAND honch_posix_e2e)
        set_tests_properties(honch_posix_e2e PROPERTIES SKIP_RETURN_CODE 77)
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
