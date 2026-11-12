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
add_compile_definitions(PW_RPC_DYNAMIC_ALLOCATION=0)

# 2. Locate Pigweed root and import its CMake utilities
set(PW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pigweed")
set(ENV{PW_ROOT} "${PW_ROOT}")

# Apply only the reviewed compatibility delta for the pinned Pigweed revision.
find_program(AVR_PYTHON3_EXECUTABLE NAMES python3 REQUIRED)
execute_process(
  COMMAND "${AVR_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/apply_pigweed_patches.py"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE _pw_patch_result
)
if(NOT _pw_patch_result EQUAL 0)
  message(FATAL_ERROR "Failed to apply pinned Pigweed AVR compatibility patch")
endif()

# Pigweed's PWPB plugin imports Python protos generated into the build tree.
# apply_pigweed_patches.py mirrors these roots into the plugin's sys.path.
# Pass those package roots to generate_protos.py through its PYTHONPATH hook.
# pw_protobuf's binary directory is build/pw_protobuf, not build/.
set(_pw_generated_python_paths
  "${CMAKE_BINARY_DIR}/pw_protobuf/pw_protobuf.codegen_protos/python"
  "${CMAKE_BINARY_DIR}/pw_protobuf/pw_protobuf.common_proto/python"
  "${CMAKE_BINARY_DIR}/pw_protobuf/pw_protobuf.status_proto/python"
  "${CMAKE_BINARY_DIR}/pw_protobuf/pw_protobuf.field_options_proto/python"
  # pw_rpc uses its own subdirectory binary root.
  "${CMAKE_BINARY_DIR}/pw_rpc/pw_rpc.protos/python"
)
if(DEFINED ENV{PYTHONPATH} AND NOT "$ENV{PYTHONPATH}" STREQUAL "")
  list(APPEND _pw_generated_python_paths "$ENV{PYTHONPATH}")
endif()
if(WIN32)
  list(JOIN _pw_generated_python_paths ";" _pw_generated_pythonpath)
else()
  list(JOIN _pw_generated_python_paths ":" _pw_generated_pythonpath)
endif()
set(ENV{PYTHONPATH} "${_pw_generated_pythonpath}")


# Architectural patch for 16-bit integer architectures (AVR: sizeof(int) == 2, sizeof(long) == 4)
set(_ARG_TYPES_FILE "${PW_ROOT}/pw_tokenizer/public/pw_tokenizer/internal/argument_types.h")
if(EXISTS "${_ARG_TYPES_FILE}")
  file(READ "${_ARG_TYPES_FILE}" _ARG_TYPES_CONTENT)
  if(_ARG_TYPES_CONTENT MATCHES "sizeof\\(ArgType\\) == sizeof\\(int64_t\\)")
    string(REPLACE
      "} else if constexpr (sizeof(ArgType) == sizeof(int64_t)) {"
      "} else if constexpr (sizeof(ArgType) > sizeof(int)) {\n    static_assert(sizeof(ArgType) <= sizeof(int64_t));"
      _ARG_TYPES_CONTENT "${_ARG_TYPES_CONTENT}")
    file(WRITE "${_ARG_TYPES_FILE}" "${_ARG_TYPES_CONTENT}")
  endif()
endif()
set(pw_unit_test_ENABLE_PW_ADD_TEST OFF CACHE BOOL "" FORCE)
include("${PW_ROOT}/pw_build/pigweed.cmake")
include("${PW_ROOT}/pw_assert/backend.cmake")
# Tokenized logging support for 8-bit AVR
add_library(pw_log INTERFACE)
target_include_directories(pw_log INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/pw_log_avr/public_overrides"
  "${CMAKE_CURRENT_SOURCE_DIR}/pw_log_avr/public"
  "${PW_ROOT}/pw_log/public"
)
target_link_libraries(pw_log INTERFACE pw_tokenizer pw_preprocessor)

# Disable -Wconversion for 8/16-bit AVR architecture (avoids false positives with 16-bit size_t)
# and disable -Wpedantic to allow standard-compliant Pigweed C++17 compilation under GCC 14/15
pw_add_library_generic(pw_build.avr_warnings INTERFACE
  PUBLIC_COMPILE_OPTIONS
    "-Wno-conversion"
    "-Wno-pedantic"
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

# 3. Declare the diagnostic pw_assert backend for AVR
pw_add_library(pw_assert_avr STATIC
  SOURCES
    pw_assert_avr/src/assert_avr.cpp
  PUBLIC_INCLUDES
    pw_assert_avr/public
  PUBLIC_DEPS
    pw_preprocessor
)

# 4. Bind assertion facades before adding subdirectories
pw_set_backend(pw_assert.check pw_assert_avr)
pw_set_backend(pw_assert.assert pw_assert_avr)

# Chrono backend for AVR
include("${PW_ROOT}/pw_chrono/backend.cmake")
pw_add_library(pw_chrono_avr.system_clock STATIC
  SOURCES
    pw_chrono_avr/src/system_clock_avr.cpp
  PUBLIC_INCLUDES
    pw_chrono_avr/public
    pw_chrono_avr/public_overrides
    ${PW_ROOT}/pw_chrono/public
  PUBLIC_DEPS
    pw_preprocessor
)
pw_set_backend(pw_chrono.system_clock pw_chrono_avr.system_clock)

# 5. Include 8-bit AVR-compatible Pigweed modules
add_subdirectory("${PW_ROOT}/third_party/fuchsia" pw_third_party_fuchsia EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_numeric" pw_numeric EXCLUDE_FROM_ALL)
add_library(pw_allocator INTERFACE)
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
add_subdirectory("${PW_ROOT}/pw_string" pw_string EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_base64" pw_base64 EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_tokenizer" pw_tokenizer EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_chrono" pw_chrono EXCLUDE_FROM_ALL)

# The AVR logging facade above replaces Pigweed's default backend, but
# pw_transfer still uses the upstream rate limiter implementation.
add_library(pw_log.rate_limited STATIC
  "${PW_ROOT}/pw_log/rate_limited.cc"
)
target_include_directories(pw_log.rate_limited PUBLIC
  "${PW_ROOT}/pw_log/public"
)
target_link_libraries(pw_log.rate_limited PUBLIC
  pw_log
  pw_chrono.system_clock
)
add_subdirectory("${PW_ROOT}/pw_stream" pw_stream EXCLUDE_FROM_ALL)
# The pinned Pigweed proto sources use proto2/proto3, not Editions. Keep
# the generator compatible with the distro protoc (3.21) by disabling the
# optional Editions flag; proto3 optional remains enabled by Pigweed.
set(pw_protobuf_compiler_GENERATE_PROTOS_ARGS "--no-experimental-editions" CACHE STRING "Extra args for Pigweed proto generation" FORCE)
add_subdirectory("${PW_ROOT}/pw_protobuf" pw_protobuf EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_checksum" pw_checksum EXCLUDE_FROM_ALL)
# Enable pw_sync interfaces
add_subdirectory("${PW_ROOT}/pw_sync" pw_sync EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_hdlc" pw_hdlc EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_rpc" pw_rpc EXCLUDE_FROM_ALL)
add_subdirectory("${PW_ROOT}/pw_transfer" pw_transfer EXCLUDE_FROM_ALL)

# Generate a real upstream raw RPC service used by the UART/HDLC integration
# probe. This exercises generated service dispatch, unary responders and
# bidirectional raw streams rather than only linking the RPC server.
pw_proto_library(avr_rpc_probe
  SOURCES
    tests/rpc_probe.proto
)

# The AVR uses a cooperative event pump, not Pigweed's blocking thread
# notification/semaphore backends. Keep all transfer state in upstream code.
function(avr_remove_target_deps target)
  foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(dependencies "${target}" "${property}")
    if(dependencies)
      foreach(dependency IN LISTS ARGN)
        list(REMOVE_ITEM dependencies "${dependency}")
      endforeach()
      set_property(TARGET "${target}" PROPERTY "${property}" "${dependencies}")
    endif()
  endforeach()
endfunction()

avr_remove_target_deps(pw_transfer.core
  pw_sync.binary_semaphore
  pw_sync.timed_thread_notification
  pw_thread.thread_core
  pw_sync.mutex
)
avr_remove_target_deps(pw_transfer pw_sync.mutex)
avr_remove_target_deps(pw_transfer.config pw_chrono.system_timer)
target_link_libraries(pw_transfer PUBLIC pw_sync.no_lock)
target_compile_definitions(pw_transfer.core PUBLIC
  PW_TRANSFER_COOPERATIVE=1
  PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY=2
)
target_compile_definitions(pw_transfer PUBLIC
  PW_TRANSFER_COOPERATIVE=1
  PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY=2
)

# Keep Pigweed RPC lockless on the single AVR owner loop.
target_link_libraries(pw_rpc.common PUBLIC
  pw_sync.no_lock
  pw_rpc.disable_global_mutex_config
)

# pw_log_avr (Tokenized logging backend for AVR)
pw_add_library(pw_log_avr STATIC
  SOURCES
    pw_log_avr/src/log_avr.cpp
  PUBLIC_INCLUDES
    pw_log_avr/public
  PUBLIC_DEPS
    pw_preprocessor
)
target_link_libraries(pw_log INTERFACE pw_log_avr)

# RPC is provided by upstream pw_rpc; no parallel AVR dispatcher is built.

# pw_crypto with pure AVR SHA-256 backend
pw_add_library(pw_crypto_avr STATIC
  SOURCES
    pw_crypto_avr/src/sha256_avr.cpp
  PUBLIC_INCLUDES
    pw_crypto_avr/public
    ${PW_ROOT}/pw_crypto/public
  PUBLIC_DEPS
    pw_bytes
    pw_status
    pw_stream
    pw_log
)
# Emit GCC stack-usage metadata so CI can verify the bounded SHA-256 frame.
target_compile_options(pw_crypto_avr PRIVATE -fstack-usage)
# ccache does not reliably preserve GCC auxiliary .su outputs; compile this
# single translation unit directly so the measurement is always generated.
set_property(TARGET pw_crypto_avr PROPERTY CXX_COMPILER_LAUNCHER "")

pw_add_library(pw_crypto INTERFACE
  PUBLIC_INCLUDES
    ${PW_ROOT}/pw_crypto/public
  PUBLIC_DEPS
    pw_crypto_avr
    pw_bytes
    pw_status
    pw_stream
    pw_log
)

# pw_ring_buffer
pw_add_library(pw_ring_buffer STATIC
  SOURCES
    ${PW_ROOT}/pw_ring_buffer/prefixed_entry_ring_buffer.cc
  PUBLIC_INCLUDES
    ${PW_ROOT}/pw_ring_buffer/public
  PUBLIC_DEPS
    pw_assert
    pw_status
    pw_result
    pw_varint
    pw_span
    pw_function
)

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

# 6a. Isolated upstream pw_rpc + Pigweed HDLC integration proof.
# Kept separate from firmware until ingress and streaming tests pass.
add_executable(pw_rpc_upstream_probe.elf EXCLUDE_FROM_ALL
  tests/pw_rpc_upstream_probe.cpp
  src/cxx_abi_stubs.cpp
)
target_include_directories(pw_rpc_upstream_probe.elf PRIVATE src)
target_link_libraries(pw_rpc_upstream_probe.elf PRIVATE
  pw_rpc.server
  pw_rpc.client
  pw_rpc.raw.server_api
  pw_rpc.raw.client_api
  avr_rpc_probe.raw_rpc
  pw_hdlc.rpc_channel_output
  pw_sync.no_lock
)
add_custom_command(TARGET pw_rpc_upstream_probe.elf POST_BUILD
  COMMAND avr-size --format=avr --mcu=${AVR_MCU} $<TARGET_FILE:pw_rpc_upstream_probe.elf>
  COMMENT "Report upstream pw_rpc AVR probe memory usage"
)

# 6b. Upstream pw_transfer cooperative AVR link probe.
# The combined client+server Transfer image does not fit the ATmega328P.
# Keep it out of the default 328P build; CI builds this target in a separate
# ATmega1284P configuration to verify the complete upstream implementation.
# The probe-only queue injector is enabled consistently for all translation
# units that include TransferThread, avoiding divergent class definitions.
if(TARGET pw_transfer.core)
  target_compile_definitions(pw_transfer.core PUBLIC
    PW_TRANSFER_COOPERATIVE_TEST=1
  )
endif()

add_executable(pw_transfer_upstream_probe.elf EXCLUDE_FROM_ALL
  tests/pw_transfer_upstream_probe.cpp
  src/cxx_abi_stubs.cpp
)
target_include_directories(pw_transfer_upstream_probe.elf PRIVATE src)
target_link_libraries(pw_transfer_upstream_probe.elf PRIVATE
  pw_transfer
  pw_rpc.server
  pw_rpc.client
  pw_rpc.raw.server_api
  pw_rpc.raw.client_api
  pw_hdlc.rpc_channel_output
  pw_sync.no_lock
)
add_custom_command(TARGET pw_transfer_upstream_probe.elf POST_BUILD
  COMMAND avr-size --format=avr --mcu=${AVR_MCU} $<TARGET_FILE:pw_transfer_upstream_probe.elf>
  COMMENT "Report upstream pw_transfer AVR probe memory usage (${AVR_MCU})"
)

# 7. Post-processing: extract .hex, assembly listing, and memory size audit
add_custom_command(TARGET firmware.elf POST_BUILD
  COMMAND avr-objcopy -O ihex -R .eeprom -R .pw_tokenizer.entries -R .pw_tokenizer.info $<TARGET_FILE:firmware.elf> firmware.hex
  COMMAND avr-objdump -h -S $<TARGET_FILE:firmware.elf> > firmware.lst
  COMMAND avr-size --format=avr --mcu=${AVR_MCU} $<TARGET_FILE:firmware.elf>
  COMMENT "Generating firmware.hex, disassembly listing, and AVR memory report (${AVR_CRC32_IMPL})"
)

# 8. Test Runner: Automated module validation suite for 8-bit AVR
add_executable(test_runner.elf
  tests/test_runner.cpp
  src/cxx_abi_stubs.cpp
)

target_include_directories(test_runner.elf PRIVATE src)

target_link_libraries(test_runner.elf PRIVATE
  pw_status
  pw_result
  pw_span
  pw_bytes
  pw_varint
  pw_containers.vector
  pw_stream
  pw_checksum
  pw_hdlc.encoder
  pw_hdlc.decoder
  pw_crypto
  pw_ring_buffer
  pw_function
  pw_string
  pw_base64
  pw_tokenizer
  pw_chrono.system_clock
  pw_log
)

add_custom_command(TARGET test_runner.elf POST_BUILD
  COMMAND avr-size --format=avr --mcu=${AVR_MCU} $<TARGET_FILE:test_runner.elf>
  COMMENT "Report of test_runner.elf memory usage"
)

add_custom_target(run_tests
  COMMAND simavr -m ${AVR_MCU} $<TARGET_FILE:test_runner.elf>
  DEPENDS test_runner.elf
  COMMENT "Running Pigweed AVR test suite in simavr"
)
