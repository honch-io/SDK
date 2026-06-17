cmake_minimum_required(VERSION 3.16)

project(honch_esp_host_tests C)

# Host-side unit tests for the ESP-IDF port's pure (dependency-free) logic.
# These compile with a plain host toolchain -- no ESP-IDF required.

add_executable(honch_esp_retry_after_test
    test_retry_after.c
    ../../honch/src/esp_retry_after.c
)

target_include_directories(honch_esp_retry_after_test PRIVATE
    ../../honch/src
)

target_compile_definitions(honch_esp_retry_after_test PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
    _DARWIN_C_SOURCE
)

target_compile_options(honch_esp_retry_after_test PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

add_executable(honch_esp_http_status_test
    test_http_status.c
    ../../honch/src/esp_http_status.c
)

target_include_directories(honch_esp_http_status_test PRIVATE
    ../../honch/src
    ../../../../core/include
)

target_compile_options(honch_esp_http_status_test PRIVATE
    $<$<C_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Werror>
)

enable_testing()
add_test(NAME honch_esp_retry_after_test COMMAND honch_esp_retry_after_test)
add_test(NAME honch_esp_http_status_test COMMAND honch_esp_http_status_test)
