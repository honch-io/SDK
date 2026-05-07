cmake_minimum_required(VERSION 3.20)

project(honch_c_core C)

option(HONCH_BUILD_TESTS "Build Honch C/POSIX SDK tests" ON)
option(HONCH_BUILD_EXAMPLES "Build Honch C/POSIX SDK examples" ON)
option(HONCH_BUILD_E2E "Build Honch C/POSIX SDK real ingest E2E test" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(HONCH_BUILD_TESTS)
    enable_testing()
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(honch_c_core
    honch/src/honch.c
    honch/src/honch_encoder.c
    honch/src/honch_json.c
    honch/src/honch_platform.c
    honch/src/honch_queue.c
    honch/src/honch_state.c
    honch/src/honch_transport_curl.c
)

target_include_directories(honch_c_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/honch/include
)

target_link_libraries(honch_c_core
    PRIVATE
        CURL::libcurl
        Threads::Threads
        ZLIB::ZLIB
)

target_compile_definitions(honch_c_core PRIVATE
    _POSIX_C_SOURCE=200809L
)

target_compile_options(honch_c_core PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_TESTS)
    target_compile_definitions(honch_c_core PRIVATE HONCH_TESTING)

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

    if(HONCH_BUILD_E2E)
        add_executable(honch_c_core_e2e
            test/test_e2e_capture.c
        )

        target_link_libraries(honch_c_core_e2e PRIVATE honch_c_core)
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
