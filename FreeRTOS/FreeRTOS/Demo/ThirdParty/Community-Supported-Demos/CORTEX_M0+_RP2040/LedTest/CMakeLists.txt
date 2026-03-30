add_executable(led_test
    main.c
)

target_include_directories(led_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(led_test
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_uart(led_test 1)
pico_enable_stdio_usb(led_test 0)

pico_add_extra_outputs(led_test)


add_executable(edf_test
    main_edf.c
)

target_include_directories(edf_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(edf_test
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_uart(edf_test 1)
pico_enable_stdio_usb(edf_test 0)
pico_add_extra_outputs(edf_test)


add_executable(edf_100test
    main_edf_100test.c
)

target_include_directories(edf_100test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(edf_100test
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_uart(edf_100test 1)
pico_enable_stdio_usb(edf_100test 0)
pico_add_extra_outputs(edf_100test)


add_executable(mp_global_test
    main_mp_global.c
)

target_include_directories(mp_global_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(mp_global_test
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_uart(mp_global_test 1)
pico_enable_stdio_usb(mp_global_test 0)
pico_add_extra_outputs(mp_global_test)


add_executable(mp_partitioned_test
    main_mp_partitioned.c
)

target_include_directories(mp_partitioned_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(mp_partitioned_test
    pico_stdlib
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_uart(mp_partitioned_test 1)
pico_enable_stdio_usb(mp_partitioned_test 0)
pico_add_extra_outputs(mp_partitioned_test)