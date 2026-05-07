add_executable(honch_connected_camera_example
    main.c
)

target_link_libraries(honch_connected_camera_example PRIVATE honch_c_core)

target_compile_options(honch_connected_camera_example PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)
