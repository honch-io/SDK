add_executable(honch_identify_merge_example
    main.c
)

target_link_libraries(honch_identify_merge_example PRIVATE honch_posix)

target_compile_options(honch_identify_merge_example PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
