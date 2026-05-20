add_library(honch_core
    src/honch_core.c
    src/honch_cbor.c
    src/honch_encoder.c
    src/honch_json.c
    src/honch_packetizer.c
    src/honch_queue_policy.c
    src/honch_support.c
)

target_include_directories(honch_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(honch_core PUBLIC c_std_11)

target_compile_options(honch_core PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_BENCHMARKS)
    target_compile_options(honch_core PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/../ports/posix/bench/honch_bench_alloc.h
    )
endif()

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

    add_executable(honch_core_retry_tests
        test/test_retry.c
        src/honch_cbor.c
        src/honch_json.c
        src/honch_queue_policy.c
        src/honch_support.c
    )

    target_include_directories(honch_core_retry_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(honch_core_retry_tests PRIVATE
        $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME honch_core_retry_tests COMMAND honch_core_retry_tests)
endif()
