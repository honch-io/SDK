add_library(honch_micropython INTERFACE) # MicroPython usermod INTERFACE_LIBRARY

target_sources(honch_micropython INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modhonch_core.c
    ${CMAKE_CURRENT_LIST_DIR}/mphal_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/mpstorage_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/mptransport_adapter.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_capture_transport.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_core.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_coredump.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_crash.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_event_record.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_lifecycle.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_packetizer.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_queue_policy.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_ram_queue.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_support.c
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src/honch_wire_v2.c
)

target_include_directories(honch_micropython INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/compat
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/include
    ${CMAKE_CURRENT_LIST_DIR}/../../../../core/src
)

target_compile_definitions(honch_micropython INTERFACE
    MICROPY_PY_HONCH_CORE=1
)

target_link_libraries(usermod INTERFACE honch_micropython)
