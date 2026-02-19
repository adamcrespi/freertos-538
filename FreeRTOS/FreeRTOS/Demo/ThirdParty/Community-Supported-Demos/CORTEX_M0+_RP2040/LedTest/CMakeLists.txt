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