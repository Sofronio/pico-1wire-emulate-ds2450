cmake_minimum_required(VERSION 3.13)

if(NOT PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH not set. Pass -DPICO_SDK_PATH=...")
    endif()
endif()

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
project(ds2450-emulator C CXX ASM)
pico_sdk_init()

add_executable(ds2450_emulator
    main.c
    ds2450_emulator.c
)

target_include_directories(ds2450_emulator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ds2450_emulator pico_stdlib hardware_gpio)
pico_add_extra_outputs(ds2450_emulator)
pico_enable_stdio_usb(ds2450_emulator 0)
pico_enable_stdio_uart(ds2450_emulator 1)
