add_executable(honch_c_core_bench
    honch_c_core_bench.c
)

target_link_libraries(honch_c_core_bench PRIVATE
    honch_c_core
    honch_bench_alloc_support
)
target_compile_definitions(honch_c_core_bench PRIVATE
    HONCH_TESTING
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    _DARWIN_C_SOURCE
)
target_compile_options(honch_c_core_bench PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
