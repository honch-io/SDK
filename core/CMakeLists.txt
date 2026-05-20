add_library(honch_core
    src/honch_core.c
    src/honch_cbor.c
    src/honch_encoder.c
    src/honch_json.c
)

target_include_directories(honch_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-core/honch/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(honch_core PUBLIC c_std_11)

target_compile_options(honch_core PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

if(HONCH_BUILD_BENCHMARKS)
    target_compile_options(honch_core PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/../c-core/bench/honch_bench_alloc.h
    )
endif()
