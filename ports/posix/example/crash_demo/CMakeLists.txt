add_executable(honch_crash_demo
    main.c
)

target_link_libraries(honch_crash_demo PRIVATE honch_posix)

target_compile_options(honch_crash_demo PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
