option(HONCH_ENABLE_ERROR_TRACKING "Compile Honch automatic error tracking support" ON)

set(HONCH_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_capture_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_coredump.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_crash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_event_record.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_lifecycle.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_packetizer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_queue_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_ram_queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_support.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/honch_wire_v2.c
)
set_property(GLOBAL PROPERTY HONCH_CORE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set_property(GLOBAL PROPERTY HONCH_CORE_SOURCES "${HONCH_CORE_SOURCES}")

function(honch_configure_core_objects target)
    get_property(HONCH_CORE_SOURCE_DIR GLOBAL PROPERTY HONCH_CORE_SOURCE_DIR)
    get_property(HONCH_CORE_SOURCES GLOBAL PROPERTY HONCH_CORE_SOURCES)

    add_library(${target} OBJECT
        ${HONCH_CORE_SOURCES}
    )

    target_include_directories(${target}
        PUBLIC
            ${HONCH_CORE_SOURCE_DIR}/include
        PRIVATE
            ${HONCH_CORE_SOURCE_DIR}/src
    )

    target_compile_features(${target} PUBLIC c_std_11)

    target_compile_definitions(${target} PUBLIC
        HONCH_ENABLE_ERROR_TRACKING=$<BOOL:${HONCH_ENABLE_ERROR_TRACKING}>
    )

    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )
endfunction()

honch_configure_core_objects(honch_core_objects)

if(HONCH_BUILD_BENCHMARKS AND NOT TARGET honch_core_bench_objects)
    honch_configure_core_objects(honch_core_bench_objects)
    target_compile_options(honch_core_bench_objects PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix/bench/honch_bench_alloc.h
    )
endif()

add_library(honch_core STATIC
    $<TARGET_OBJECTS:honch_core_objects>
)

target_link_libraries(honch_core
    PUBLIC
        honch_core_objects
)

if(HONCH_BUILD_TESTS)
    add_executable(honch_core_storage_contract_tests
        test/test_storage_contract.c
    )

    target_link_libraries(honch_core_storage_contract_tests PRIVATE honch_core)
    target_compile_options(honch_core_storage_contract_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_storage_contract_tests COMMAND honch_core_storage_contract_tests)

    add_executable(honch_core_packetizer_tests
        test/test_packetizer.c
    )

    target_link_libraries(honch_core_packetizer_tests PRIVATE honch_core)
    target_include_directories(honch_core_packetizer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(honch_core_packetizer_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_packetizer_tests COMMAND honch_core_packetizer_tests)

    add_executable(honch_core_wire_v2_tests
        test/test_wire_v2.c
    )

    target_link_libraries(honch_core_wire_v2_tests PRIVATE honch_core)
    target_compile_options(honch_core_wire_v2_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_wire_v2_tests COMMAND honch_core_wire_v2_tests)

    add_executable(honch_core_coredump_tests
        test/test_coredump.c
    )

    target_link_libraries(honch_core_coredump_tests PRIVATE honch_core)
    target_include_directories(honch_core_coredump_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(honch_core_coredump_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_coredump_tests COMMAND honch_core_coredump_tests)

    add_executable(honch_core_capture_transport_tests
        test/test_capture_transport.c
    )

    target_link_libraries(honch_core_capture_transport_tests PRIVATE honch_core)
    target_compile_options(honch_core_capture_transport_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_capture_transport_tests COMMAND honch_core_capture_transport_tests)

    add_executable(honch_core_state_consistency_tests
        test/test_state_consistency.c
        ../ports/posix/src/posix_platform.c
        ../ports/posix/src/posix_state.c
        ../ports/posix/src/posix_storage.c
    )

    target_link_libraries(honch_core_state_consistency_tests PRIVATE honch_core)
    target_include_directories(honch_core_state_consistency_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix/src
    )
    target_compile_options(honch_core_state_consistency_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_state_consistency_tests COMMAND honch_core_state_consistency_tests)

    add_executable(honch_core_retry_tests
        test/test_retry.c
        src/honch_event_record.c
        src/honch_lifecycle.c
        src/honch_queue_policy.c
        src/honch_support.c
        src/honch_wire_v2.c
    )

    target_include_directories(honch_core_retry_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(honch_core_retry_tests PRIVATE
        HONCH_TESTING
    )
    target_compile_options(honch_core_retry_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_retry_tests COMMAND honch_core_retry_tests)
endif()
