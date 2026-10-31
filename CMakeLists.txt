# File: CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(avr_pigweed_firmware LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. CRC-32 algorithm selection for pw_checksum / pw_hdlc on AVR:
#    - PW_CHECKSUM_CRC32_4BITS : 16-entry table (64 B). Saves 960 B of SRAM and 960 B of Flash.
#    - PW_CHECKSUM_CRC32_1BITS : No table (0 B). Saves 1024 B of SRAM and ~976 B of Flash.
#    - PW_CHECKSUM_CRC32_8BITS : 256-entry table (1024 B). Not recommended on ATmega328P.
set(AVR_CRC32_IMPL "PW_CHECKSUM_CRC32_4BITS" CACHE STRING
    "CRC32 variant for pw_checksum (PW_CHECKSUM_CRC32_4BITS or PW_CHECKSUM_CRC32_1BITS)")

# Define compile definitions globally before evaluating Pigweed subdirectories
add_compile_definitions(PW_CHECKSUM_CRC32_DEFAULT_IMPL=${AVR_CRC32_IMPL})
add_compile_definitions(__cpp_lib_endian=201907L)

# 2. Locate Pigweed root and import its CMake utilities
set(PW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pigweed")
set(ENV{PW_ROOT} "${PW_ROOT}")
set(pw_unit_test_ENABLE_PW_ADD_TEST OFF CACHE BOOL "" FORCE)
include("${PW_ROOT}/pw_build/pigweed.cmake")
include("${PW_ROOT}/pw_assert/backend.cmake")

# Disable -Wconversion for 8/16-bit AVR architecture (avoids false positives with 16-bit size_t)
pw_add_library_generic(pw_build.avr_warnings INTERFACE
  PUBLIC_COMPILE_OPTIONS
    "-Wno-conversion"
)
set(pw_build_WARNINGS
    pw_build.strict_warnings
    pw_build.extra_strict_warnings
    pw_build.internal_strict_warnings
    pw_build.avr_warnings
    CACHE STRING "" FORCE)

# Disable strict deferred dependency check for unused modules
function(_pw_target_link_targets_deferred_check NAME TYPE LIBRARY)
endfunction()

add_subdirectory("${PW_ROOT}/pw_build" pw_build EXCLUDE_FROM_ALL)

# 3. Declare the minimalist pw_assert backend for AVR
pw_add_library(pw_assert_avr INTERFACE
  PUBLIC_INCLUDES
    pw_assert_avr/public
    pw_assert_avr/public_overrides
  PUBLIC_DEPS
    pw_preprocessor
)

# 4. Bind assertion facades before adding subdirectories
pw_set_backend(pw_assert.check pw_assert_avr)
pw_set_backend(pw_assert.assert pw_assert_avr)

# 5. Include only 8-bit AVR-compatible Pigweed modules
add_subdirectory("${PW_ROOT}/third_party/fuchsia" pw_third_party_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_numeric" pw_numeric EXCLUDE_FROM_ALL)
add_library(pw_allocator INTERFACE)
add_library(pw_tokenizer INTERFACE)
add_library(pw_unit_test INTERFACE)
add_library(pw_toolchain.constexpr_tag INTERFACE)
target_include_directories(pw_toolchain.constexpr_tag INTERFACE "${PW_ROOT}/pw_toolchain/public")
add_subdirectory("${PW_ROOT}/pw_enum" pw_enum EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_function" pw_function EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_memory" pw_memory EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_polyfill" pw_polyfill EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_preprocessor" pw_preprocessor EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_assert" pw_assert EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_status" pw_status EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_result" pw_result EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_span" pw_span EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_bytes" pw_bytes EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_varint" pw_varint EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_containers" pw_containers EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_stream" pw_stream EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_checksum" pw_checksum EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_hdlc" pw_hdlc EXCLUDE_FROM_ALL)

# Ensure pw_checksum configuration target exports macro to pw_hdlc
if(TARGET pw_checksum)
  target_compile_definitions(pw_checksum PUBLIC
    PW_CHECKSUM_CRC32_DEFAULT_IMPL=${AVR_CRC32_IMPL}
  )
endif()

# 6. Define firmware binary
add_executable(firmware.elf
  src/main.cpp
  src/cxx_abi_stubs.cpp
)

target_include_directories(firmware.elf PRIVATE src)

target_link_libraries(firmware.elf PRIVATE
  pw_status
  pw_result
  pw_span
  pw_bytes
  pw_varint
  pw_containers.vector
  pw_stream
  pw_checksum
  pw_hdlc.encoder
)

# 7. Post-processing: extract .hex, assembly listing, and memory size audit
add_custom_command(TARGET firmware.elf POST_BUILD
  COMMAND avr-objcopy -O ihex -R .eeprom -R .pw_tokenizer.entries -R .pw_tokenizer.info $<TARGET_FILE:firmware.elf> firmware.hex
  COMMAND avr-objdump -h -S $<TARGET_FILE:firmware.elf> > firmware.lst
  COMMAND avr-size --format=avr --mcu=${AVR_MCU} $<TARGET_FILE:firmware.elf>
  COMMENT "Generating firmware.hex, disassembly listing, and AVR memory report (${AVR_CRC32_IMPL})"
)
