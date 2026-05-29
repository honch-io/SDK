add_executable(honch_posix_bench
    honch_posix_bench.c
)

target_link_libraries(honch_posix_bench PRIVATE
    honch_posix_bench_instrumented
    honch_bench_alloc_support
)
target_compile_definitions(honch_posix_bench PRIVATE
    HONCH_TESTING
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    _DARWIN_C_SOURCE
)
target_compile_options(honch_posix_bench PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_TESTS)
    add_test(
        NAME honch_posix_rate_sweep_bench
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_rate_sweep_output.py
            --binary $<TARGET_FILE:honch_posix_bench>
    )
endif()
