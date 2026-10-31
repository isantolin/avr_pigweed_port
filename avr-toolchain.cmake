# File: avr-toolchain.cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR avr)

set(CMAKE_C_COMPILER avr-gcc)
set(CMAKE_CXX_COMPILER avr-g++)
set(CMAKE_ASM_COMPILER avr-gcc)
set(CMAKE_AR avr-gcc-ar)
set(CMAKE_RANLIB avr-gcc-ranlib)

# Target microcontroller parameters (can be overridden via command line)
set(AVR_MCU "atmega328p" CACHE STRING "Target AVR microcontroller")
set(AVR_F_CPU "16000000UL" CACHE STRING "MCU clock frequency in Hz")

set(AVR_STDLIB_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/third_party/avr-libstdcpp/include")

set(COMMON_AVR_FLAGS "-mmcu=${AVR_MCU} -DF_CPU=${AVR_F_CPU} -Os -ffunction-sections -fdata-sections -flto")

set(CMAKE_C_FLAGS_INIT "${COMMON_AVR_FLAGS} -Wall -Wextra -Wno-conversion")
set(CMAKE_CXX_FLAGS_INIT "${COMMON_AVR_FLAGS} -Wall -Wextra -Wno-conversion -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -D__cpp_lib_endian=201907L -isystem ${AVR_STDLIB_INCLUDE}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mmcu=${AVR_MCU} -Wl,--gc-sections -flto")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
