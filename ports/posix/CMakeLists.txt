cmake_minimum_required(VERSION 3.20)

project(honch_posix_sdk VERSION 0.2.4 LANGUAGES C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(HONCH_BUILD_TESTS "Build Honch POSIX SDK tests" ON)
option(HONCH_BUILD_EXAMPLES "Build Honch POSIX SDK examples" ON)
option(HONCH_BUILD_BENCHMARKS "Build Honch POSIX SDK benchmarks" OFF)
option(HONCH_BUILD_E2E "Build Honch POSIX SDK end-to-end tests" OFF)
option(HONCH_ENABLE_ERROR_TRACKING "Compile Honch automatic error tracking support" ON)
# Sub-toggles cascade from the umbrella unless explicitly set on the command
# line. Re-derived plain vars (NOT option()/cache): option() evaluates its
# default only when the cache entry is first created, so flipping the umbrella
# in an existing build dir would not re-derive these (a silent mis-strip).
if(NOT DEFINED HONCH_ENABLE_CRASH_CAPTURE)
    set(HONCH_ENABLE_CRASH_CAPTURE ${HONCH_ENABLE_ERROR_TRACKING})
endif()
if(NOT DEFINED HONCH_ENABLE_LOG_CAPTURE)
    set(HONCH_ENABLE_LOG_CAPTURE ${HONCH_ENABLE_ERROR_TRACKING})
endif()
option(HONCH_ENABLE_LIFECYCLE_EVENTS "Compile Honch auto lifecycle events ($device_boot/$firmware_update/$device_shutdown)" ON)
option(HONCH_ENABLE_SESSIONS "Compile Honch session tracking ($session_start/$session_end)" ON)

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

if(HONCH_BUILD_BENCHMARKS AND NOT TARGET honch_core_bench_objects)
    if(NOT COMMAND honch_configure_core_objects)
        message(FATAL_ERROR "HONCH_BUILD_BENCHMARKS requires honch_configure_core_objects from the core CMake target")
    endif()
    honch_configure_core_objects(honch_core_bench_objects)
    target_compile_options(honch_core_bench_objects PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/bench/honch_bench_alloc.h
    )
endif()

set(HONCH_POSIX_SOURCES
    src/posix_compat.c
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
    $<TARGET_OBJECTS:honch_core_objects>
)

set(HONCH_POSIX_BENCH_SOURCES
    src/posix_compat.c
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
    $<TARGET_OBJECTS:honch_core_bench_objects>
)

function(honch_configure_posix_target target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../core/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )

    target_link_libraries(${target}
        PRIVATE
            CURL::libcurl
            Threads::Threads
    )

    target_compile_definitions(${target} PRIVATE
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
        _POSIX_C_SOURCE=200809L
    )

    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
endfunction()

add_library(honch_posix ${HONCH_POSIX_SOURCES})
honch_configure_posix_target(honch_posix)

if(HONCH_BUILD_TESTS)
    # Build a second copy of the core objects with HONCH_TESTING so the test-only
    # instrumentation (e.g. the encode-attempt counter) is available to the test
    # binaries while never being compiled into the production honch_posix lib.
    if(NOT TARGET honch_core_test_objects)
        honch_configure_core_objects(honch_core_test_objects)
        target_compile_definitions(honch_core_test_objects PRIVATE HONCH_TESTING)
    endif()

    set(HONCH_POSIX_TEST_SOURCES
        src/posix_compat.c
        src/posix_platform.c
        src/posix_storage.c
        src/posix_state.c
        src/posix_transport_curl.c
        $<TARGET_OBJECTS:honch_core_test_objects>
    )

    add_library(honch_posix_test ${HONCH_POSIX_TEST_SOURCES})
    honch_configure_posix_target(honch_posix_test)
    target_compile_definitions(honch_posix_test PRIVATE
        HONCH_TESTING
    )

    add_executable(honch_posix_tests
        test/test_honch.c
    )

    target_link_libraries(honch_posix_tests PRIVATE honch_posix_test)
    # core/src so the test can assert the wire $sdk_version against the canonical
    # HONCH_SDK_VERSION macro instead of a hardcoded literal.
    target_include_directories(honch_posix_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )
    target_compile_definitions(honch_posix_tests PRIVATE
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
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
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_state_consistency_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_posix_state_consistency_tests COMMAND honch_posix_state_consistency_tests)

    add_executable(honch_posix_durable_delete_tests
        test/test_durable_delete.c
    )
    target_link_libraries(honch_posix_durable_delete_tests PRIVATE honch_posix_test)
    target_include_directories(honch_posix_durable_delete_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )
    target_compile_definitions(honch_posix_durable_delete_tests PRIVATE
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
        HONCH_TESTING
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_durable_delete_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME honch_posix_durable_delete_tests COMMAND honch_posix_durable_delete_tests)

    add_executable(honch_posix_lifecycle_toggle_tests
        test/test_lifecycle_toggle.c
    )
    target_link_libraries(honch_posix_lifecycle_toggle_tests PRIVATE honch_posix_test)
    target_include_directories(honch_posix_lifecycle_toggle_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )
    target_compile_definitions(honch_posix_lifecycle_toggle_tests PRIVATE
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
        HONCH_TESTING
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_lifecycle_toggle_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME honch_posix_lifecycle_toggle_tests COMMAND honch_posix_lifecycle_toggle_tests)

    add_executable(honch_posix_session_toggle_tests
        test/test_session_toggle.c
    )
    target_link_libraries(honch_posix_session_toggle_tests PRIVATE honch_posix_test)
    target_include_directories(honch_posix_session_toggle_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src
    )
    target_compile_definitions(honch_posix_session_toggle_tests PRIVATE
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
        HONCH_ENABLE_CRASH_CAPTURE=$<BOOL:${HONCH_ENABLE_CRASH_CAPTURE}>
        HONCH_ENABLE_LOG_CAPTURE=$<BOOL:${HONCH_ENABLE_LOG_CAPTURE}>
        HONCH_ENABLE_LIFECYCLE_EVENTS=$<BOOL:${HONCH_ENABLE_LIFECYCLE_EVENTS}>
        HONCH_ENABLE_SESSIONS=$<BOOL:${HONCH_ENABLE_SESSIONS}>
        HONCH_TESTING
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE
        _DARWIN_C_SOURCE
    )
    target_compile_options(honch_posix_session_toggle_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME honch_posix_session_toggle_tests COMMAND honch_posix_session_toggle_tests)

    add_test(
        NAME honch_posix_sdk_contract_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_sdk_contract.py
    )

    add_test(
        NAME honch_posix_tls_config_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/test_tls_config.py
    )

    if(HONCH_BUILD_E2E)
        add_executable(honch_posix_e2e
            test/test_e2e_capture.c
        )

        target_link_libraries(honch_posix_e2e PRIVATE honch_posix_test)
        if(UNIX AND NOT APPLE)
            target_link_libraries(honch_posix_e2e PRIVATE m)
        endif()
        target_compile_definitions(honch_posix_e2e PRIVATE
            HONCH_TESTING
            _POSIX_C_SOURCE=200809L
            _DEFAULT_SOURCE
            _DARWIN_C_SOURCE
        )
        target_compile_options(honch_posix_e2e PRIVATE
            $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
        )

        add_test(NAME honch_posix_e2e COMMAND honch_posix_e2e)
    endif()
endif()

if(HONCH_BUILD_EXAMPLES)
    add_subdirectory(example/posix_device)
    add_subdirectory(example/connected_camera)
    add_subdirectory(example/posix_gpio)
    add_subdirectory(example/identify_merge)
    add_subdirectory(example/crash_demo)
endif()

if(HONCH_BUILD_BENCHMARKS)
    add_library(honch_posix_bench_instrumented ${HONCH_POSIX_BENCH_SOURCES})
    honch_configure_posix_target(honch_posix_bench_instrumented)
    target_compile_definitions(honch_posix_bench_instrumented PRIVATE
        HONCH_TESTING
    )
    target_compile_options(honch_posix_bench_instrumented PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/bench/honch_bench_alloc.h
    )
    target_link_libraries(honch_posix_bench_instrumented PRIVATE
        honch_bench_alloc_support
    )

    add_subdirectory(bench)
endif()

set(HONCH_POSIX_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/honch_posix")

install(TARGETS honch_posix
    EXPORT honch_posixTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/honch_posixConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/honch_posixConfig.cmake
    INSTALL_DESTINATION ${HONCH_POSIX_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/honch_posixConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT honch_posixTargets
    NAMESPACE honch::
    DESTINATION ${HONCH_POSIX_INSTALL_CMAKEDIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/honch_posixConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/honch_posixConfigVersion.cmake
    DESTINATION ${HONCH_POSIX_INSTALL_CMAKEDIR}
)
