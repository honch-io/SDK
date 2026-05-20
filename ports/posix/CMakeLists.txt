add_library(honch_posix
    src/posix_platform.c
    src/posix_storage.c
    src/posix_state.c
    src/posix_transport_curl.c
)

target_include_directories(honch_posix
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../c-core/honch/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../c-core/honch/src
)

target_link_libraries(honch_posix
    PUBLIC
        honch_core
    PRIVATE
        CURL::libcurl
        Threads::Threads
)

if(ZLIB_FOUND)
    target_link_libraries(honch_posix PRIVATE ZLIB::ZLIB)
    target_compile_definitions(honch_posix PRIVATE HONCH_HAVE_ZLIB)
endif()

target_compile_definitions(honch_posix PRIVATE
    _POSIX_C_SOURCE=200809L
)

if(HONCH_BUILD_TESTS OR HONCH_BUILD_BENCHMARKS)
    target_compile_definitions(honch_posix PRIVATE HONCH_TESTING)
endif()

if(HONCH_BUILD_BENCHMARKS)
    target_compile_options(honch_posix PRIVATE
        -include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../c-core/bench/honch_bench_alloc.h
    )
endif()

target_compile_options(honch_posix PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
