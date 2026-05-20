add_executable(honch_posix_example
    main.c
)

target_link_libraries(honch_posix_example PRIVATE honch_posix)

target_compile_options(honch_posix_example PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
