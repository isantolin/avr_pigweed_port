cmake_minimum_required(VERSION 3.20)
project(pw_emu_avr_simavr_runner C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

find_path(SIMAVR_INCLUDE_DIR sim_avr.h PATH_SUFFIXES simavr REQUIRED)
find_library(SIMAVR_LIBRARY NAMES simavr REQUIRED)
find_library(UTIL_LIBRARY NAMES util)

add_executable(simavr_pty_runner simavr_pty_runner.c)
target_include_directories(simavr_pty_runner SYSTEM PRIVATE ${SIMAVR_INCLUDE_DIR})
target_link_libraries(simavr_pty_runner PRIVATE ${SIMAVR_LIBRARY})
if(UTIL_LIBRARY)
  target_link_libraries(simavr_pty_runner PRIVATE ${UTIL_LIBRARY})
endif()
target_compile_options(simavr_pty_runner PRIVATE
  -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion
)

find_program(AVR_GCC_EXECUTABLE avr-gcc REQUIRED)
set(PW_EMU_TEST_FIRMWARE ${CMAKE_CURRENT_BINARY_DIR}/pw_emu_test_firmware.elf)
add_custom_command(
  OUTPUT ${PW_EMU_TEST_FIRMWARE}
  COMMAND ${AVR_GCC_EXECUTABLE}
    -mmcu=atmega328p
    -DF_CPU=16000000UL
    -std=gnu11
    -Os
    -Wall -Wextra -Werror
    ${CMAKE_CURRENT_SOURCE_DIR}/test_firmware.c
    -o ${PW_EMU_TEST_FIRMWARE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_firmware.c
  VERBATIM
)
add_custom_target(pw_emu_test_firmware ALL DEPENDS ${PW_EMU_TEST_FIRMWARE})
