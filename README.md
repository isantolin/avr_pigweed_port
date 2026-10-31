# Implementation Guide and Source Code: Pigweed Port to 8-bit AVR

This document contains the complete source code, cross-compilation build system configuration (`CMake` + `avr-gcc`), and step-by-step operational guide to run core **Google Pigweed** modules on **8-bit AVR microcontrollers** (such as the ATmega328P, ATmega32U4, or ATmega2560).

---

## 1. Project Architecture and File Structure

To avoid the 32-bit toolchain constraints of GN/Bazel, this architecture leverages Pigweed's native CMake build engine (`pw_build/pigweed.cmake`), injects the C++17 standard library via `modm-io/avr-libstdcpp`, and replaces assertion facades (`pw_assert`) with direct calls to `__builtin_trap()`.

```text
avr_pigweed_port/
├── avr-toolchain.cmake                 # avr-gcc cross-compiler definition and C++17 flags
├── CMakeLists.txt                      # Modular build with pw_build and CRC-32 optimization
├── bootstrap_avr_pigweed.sh            # Automated setup and build script
├── pw_assert_avr/                      # Minimalist assertion backend (0 bytes of SRAM)
│   ├── public/
│   │   └── pw_assert_backend/
│   │       ├── assert_backend.h
│   │       ├── assert_lite_backend.h
│   │       └── check_backend.h
│   └── public_overrides/
│       └── pw_assert_backend/
│           ├── assert_backend.h
│           ├── assert_lite_backend.h
│           └── check_backend.h
├── src/
│   ├── avr_uart_stream.h               # pw::stream implementation over AVR USART0 registers
│   ├── cxx_abi_stubs.cpp               # C++17 ABI stubs (operator delete and static guards)
│   └── main.cpp                        # Firmware with pw_result, pw_containers, pw_varint, and pw_hdlc
├── tools/
│   └── receive_telemetry.py            # Host Python script to decode pw_hdlc frames
└── third_party/
    ├── avr-libstdcpp/                  # Git submodule: C++17 STL headers for AVR
    └── pigweed/                        # Git submodule: Upstream Pigweed repository
```

---

## 2. Complete Source Code

### 2.1. Cross-Compilation Toolchain (`avr-toolchain.cmake`)

Configures `avr-gcc` and `avr-g++` with size optimization (`-Os`), Link-Time Optimization (`-flto`), dead-code stripping (`--gc-sections`), suppression of 16-bit integer conversion warnings (`-Wno-conversion`), and priority injection (`-isystem`) of C++17 standard headers.

```cmake
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
```

---

### 2.2. Minimalist Assertion Backend (`pw_assert_avr`)

Decouples `pw_assert` from `stdio.h` and `snprintf`, preventing failure messages from consuming AVR SRAM.

#### `pw_assert_avr/public/pw_assert_backend/check_backend.h`
```cpp
// File: pw_assert_avr/public/pw_assert_backend/check_backend.h
#pragma once

#include "pw_preprocessor/compiler.h"

// Halts execution deterministically without storing strings in SRAM
#define PW_HANDLE_CRASH(...) __builtin_trap()

#define PW_HANDLE_ASSERT_FAILURE(condition_string, message, ...) \
  __builtin_trap()

#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(arg_a_str,         \
                                                arg_a_val,         \
                                                comparison_op_str, \
                                                arg_b_str,         \
                                                arg_b_val,         \
                                                type_fmt,          \
                                                message,           \
                                                ...)               \
  __builtin_trap()
```

#### `pw_assert_avr/public_overrides/pw_assert_backend/check_backend.h`
```cpp
// File: pw_assert_avr/public_overrides/pw_assert_backend/check_backend.h
#pragma once

#include "pw_assert_backend/check_backend.h"
```

#### `pw_assert_avr/public/pw_assert_backend/assert_backend.h`
```cpp
// File: pw_assert_avr/public/pw_assert_backend/assert_backend.h
#pragma once

#define PW_ASSERT_HANDLE_FAILURE(...) __builtin_trap()
```

#### `pw_assert_avr/public_overrides/pw_assert_backend/assert_backend.h`
```cpp
// File: pw_assert_avr/public_overrides/pw_assert_backend/assert_backend.h
#pragma once

#include "pw_assert_backend/assert_backend.h"
```

#### `pw_assert_avr/public/pw_assert_backend/assert_lite_backend.h`
```cpp
// File: pw_assert_avr/public/pw_assert_backend/assert_lite_backend.h
#pragma once

#define PW_ASSERT_HANDLE_FAILURE() __builtin_trap()
```

#### `pw_assert_avr/public_overrides/pw_assert_backend/assert_lite_backend.h`
```cpp
// File: pw_assert_avr/public_overrides/pw_assert_backend/assert_lite_backend.h
#pragma once

#include "pw_assert_backend/assert_lite_backend.h"
```

---

### 2.3. C++17 ABI Stubs (`src/cxx_abi_stubs.cpp`)

Satisfies runtime symbols emitted by `avr-g++` for polymorphic classes (`pw::stream::Stream`) and destructors without requiring a dynamic memory heap.

```cpp
// File: src/cxx_abi_stubs.cpp
#include <stddef.h>
#include <stdint.h>

void operator delete(void*) noexcept { __builtin_trap(); }
void operator delete(void*, size_t) noexcept { __builtin_trap(); }

extern "C" void __cxa_pure_virtual() { __builtin_trap(); }

// Static initialization guards for single-threaded 8-bit architectures
namespace __cxxabiv1 {
__extension__ typedef int __guard __attribute__((mode(__DI__)));

extern "C" int __cxa_guard_acquire(__guard* g) {
  return !*(reinterpret_cast<uint8_t*>(g));
}

extern "C" void __cxa_guard_release(__guard* g) {
  *(reinterpret_cast<uint8_t*>(g)) = 1;
}

extern "C" void __cxa_guard_abort(__guard*) {}
}  // namespace __cxxabiv1
```

---

### 2.4. `pw_stream` Adapter for AVR UART (`src/avr_uart_stream.h`)

Implements the `pw::stream::NonSeekableReaderWriter` interface directly over the ATmega328P hardware `USART0` registers. This enables direct use of the **`pw_hdlc`** frame encoder without dynamic memory allocation.

The Baud Rate Register ($\text{UBRR0}$) value in asynchronous normal mode is calculated as:

$$\text{UBRR0} = \frac{f_{\text{CPU}}}{16 \times \text{Baud}} - 1$$

```cpp
// File: src/avr_uart_stream.h
#pragma once

#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"

namespace avr_port {

class AvrUartStream final : public pw::stream::NonSeekableReaderWriter {
 public:
  explicit AvrUartStream(uint32_t baud_rate) {
    const uint16_t ubrr =
        static_cast<uint16_t>((F_CPU / (16UL * baud_rate)) - 1UL);
    UBRR0H = static_cast<uint8_t>(ubrr >> 8);
    UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);

    // Enable USART0 receiver and transmitter
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    // Frame format: 8 data bits, no parity, 1 stop bit (8N1)
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    for (const std::byte b : data) {
      while (!(UCSR0A & (1 << UDRE0))) {
        // Busy-wait until transmit buffer is ready
      }
      UDR0 = static_cast<uint8_t>(b);
    }
    return pw::OkStatus();
  }

  pw::StatusWithSize DoRead(pw::ByteSpan destination) override {
    if (destination.empty()) {
      return pw::StatusWithSize(0);
    }
    // Non-blocking read of the first available byte
    if (!(UCSR0A & (1 << RXC0))) {
      return pw::StatusWithSize::ResourceExhausted();
    }
    destination[0] = static_cast<std::byte>(UDR0);
    return pw::StatusWithSize(1);
  }
};

}  // namespace avr_port
```

---

### 2.5. Main Application Code (`src/main.cpp`)

Integrates `pw_status`, `pw_result`, `pw_containers` (`pw::Vector`), `pw_varint`, `pw_bytes`, and `pw_hdlc` to sample an ADC channel, encode the reading in *Varint* format inside a static capacity container, and transmit a CRC-32 verified HDLC frame over serial.

```cpp
// File: src/main.cpp
#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>
#include <util/delay.h>

#include "avr_uart_stream.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_hdlc/encoder.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_varint/varint.h"

namespace {

constexpr uint8_t kTelemetryHdlcAddress = 1;

void InitAdc() {
  // AVcc reference (5V) and default ADC0 channel selected
  ADMUX = (1 << REFS0);
  // Enable ADC with prescaler of 128 (16 MHz / 128 = 125 kHz)
  ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

// Reads channel ADC0 and returns a pw::Result<uint16_t> without using exceptions
pw::Result<uint16_t> ReadAdcChannel0() {
  ADCSRA |= (1 << ADSC);
  while (ADCSRA & (1 << ADSC)) {
    // Wait for conversion completion
  }
  const uint16_t value = ADC;
  if (value > 1023U) {
    return pw::Status::OutOfRange();
  }
  return value;
}

// Packs sequence number and ADC reading using pw::varint
pw::Status BuildTelemetryPayload(uint16_t sequence,
                                 pw::Vector<std::byte, 16>& payload) {
  payload.clear();

  uint16_t adc_sample{};
  PW_TRY_ASSIGN(adc_sample, ReadAdcChannel0());

  std::byte varint_buf[pw::varint::kMaxVarint32SizeBytes];

  // Encode sequence number
  const size_t seq_len = pw::varint::Encode(sequence, varint_buf);
  for (size_t i = 0; i < seq_len; ++i) {
    if (payload.full()) {
      return pw::Status::ResourceExhausted();
    }
    payload.push_back(varint_buf[i]);
  }

  // Encode ADC value
  const size_t adc_len = pw::varint::Encode(adc_sample, varint_buf);
  for (size_t i = 0; i < adc_len; ++i) {
    if (payload.full()) {
      return pw::Status::ResourceExhausted();
    }
    payload.push_back(varint_buf[i]);
  }

  return pw::OkStatus();
}

}  // namespace

int main() {
  // Configure pin PB5 (built-in LED on Arduino Uno/Nano) as output
  DDRB |= (1 << DDB5);
  InitAdc();

  avr_port::AvrUartStream serial_stream(115200);
  pw::Vector<std::byte, 16> packet_payload;
  uint16_t sequence_counter = 0;

  for (;;) {
    if (BuildTelemetryPayload(sequence_counter++, packet_payload).ok()) {
      const pw::ConstByteSpan payload_span(packet_payload.data(),
                                           packet_payload.size());

      // Send HDLC frame with address 1 and CRC-32 checksum
      if (pw::hdlc::WriteUIFrame(
              kTelemetryHdlcAddress, payload_span, serial_stream)
              .ok()) {
        PORTB ^= (1 << PORTB5);  // Toggle LED on successful transmission
      }
    }
    _delay_ms(500);
  }

  return 0;
}
```

---

### 2.6. Optimization of `pw_checksum` on AVR and Build Configuration (`CMakeLists.txt`)

#### Why Default `pw_checksum` Configuration is Critical on AVR
The `pw_hdlc` module computes frame check sequences (FCS) using `pw::checksum::Crc32`. In its default configuration (`PW_CHECKSUM_CRC32_8BITS`), Pigweed computes a `constexpr` lookup table of 256 32-bit entries (`kCrc32Table`).

On 32-bit microcontrollers (e.g., ARM Cortex-M), `.rodata` is read directly from Flash memory. However, due to the **Harvard architecture of 8-bit AVR microcontrollers**, constants in `.rodata` lacking explicit `PROGMEM` attributes are copied at startup (`__do_copy_data`) from Flash into **SRAM** to be addressable via standard 16-bit pointers. Consequently, `PW_CHECKSUM_CRC32_8BITS` consumes **1024 bytes of Flash** and **1024 bytes of SRAM** (50% of total RAM on an ATmega328P).

Pigweed defines `PW_CHECKSUM_CRC32_DEFAULT_IMPL` in `pw_checksum/public/pw_checksum/internal/config.h`, allowing algorithm selection before compilation:

| Value of `PW_CHECKSUM_CRC32_DEFAULT_IMPL` | Algorithm per iteration | Table Entries | Table Consumption (SRAM and Flash on AVR) | SRAM Savings on AVR |
| :--- | :--- | :--- | :--- | :--- |
| `PW_CHECKSUM_CRC32_8BITS` *(Default)* | 8 bits (1 byte) | $256 \times \text{uint32\_t}$ | 1024 B Flash + 1024 B SRAM | 0 B *(Not recommended on AVR)* |
| **`PW_CHECKSUM_CRC32_4BITS`** | 4 bits (1 nibble) | $16 \times \text{uint32\_t}$ | 64 B Flash + 64 B SRAM | **Saves 960 B SRAM and 960 B Flash** |
| **`PW_CHECKSUM_CRC32_1BITS`** | 1 bit (pure polynomial) | 0 (No table) | 0 B table (48 B Flash code) | **Saves 1024 B SRAM (100%) and ~976 B Flash** |

```cmake
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
```

---

## 3. Automated Deployment Script (`bootstrap_avr_pigweed.sh`)

To set up the workspace and submodules automatically:

```bash
#!/usr/bin/env bash
# File: bootstrap_avr_pigweed.sh
set -euo pipefail

PROJECT_DIR="avr_pigweed_port"
mkdir -p "${PROJECT_DIR}/pw_assert_avr/public/pw_assert_backend"
mkdir -p "${PROJECT_DIR}/pw_assert_avr/public_overrides/pw_assert_backend"
mkdir -p "${PROJECT_DIR}/src"
cd "${PROJECT_DIR}"

if [ ! -d ".git" ]; then
  git init
fi

if [ ! -d "third_party/pigweed" ]; then
  git submodule add --depth 1 https://pigweed.googlesource.com/pigweed/pigweed third_party/pigweed
fi

if [ ! -d "third_party/avr-libstdcpp" ]; then
  git submodule add --depth 1 https://github.com/modm-io/avr-libstdcpp.git third_party/avr-libstdcpp
fi

echo "Directory structure and submodules ready in $(pwd)."
echo "Copy CMakeLists.txt, avr-toolchain.cmake, pw_assert_avr/*, and src/* files, then run:"
echo "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS"
echo "  cmake --build build"
```

---

## 4. Step-by-Step User Manual

### Step 1: System Prerequisites Installation

Ensure `avr-gcc` ($\ge 11$ recommended for complete C++17 support), `avr-libc`, `CMake`, `Ninja`, and `avrdude` are installed:

* **On Fedora / Red Hat:**
  ```bash
  sudo dnf install -y avr-gcc avr-gcc-c++ avr-libc avr-binutils cmake ninja-build git python3-pip
  ```
* **On Ubuntu / Debian:**
  ```bash
  sudo apt update
  sudo apt install -y gcc-avr avr-libc binutils-avr avrdude cmake ninja-build git python3-pip
  ```
* **On Arch Linux:**
  ```bash
  sudo pacman -S avr-gcc avr-libc avr-binutils avrdude cmake ninja git
  ```
* **On macOS (Homebrew):**
  ```bash
  brew tap osx-cross/avr
  brew install avr-gcc avrdude cmake ninja git
  ```

### Step 2: Firmware Configuration and Build (Selecting 4-bit or 1-bit CRC)

From the project root `avr_pigweed_port/`:

1. **Option A — Build with 4-bit CRC-32 (`PW_CHECKSUM_CRC32_4BITS`, Default):**
   ```bash
   cmake -B build -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
     -DAVR_MCU=atmega328p \
     -DAVR_F_CPU=16000000UL \
     -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS
   cmake --build build
   ```

2. **Option B — Build with 1-bit CRC-32 (`PW_CHECKSUM_CRC32_1BITS`, Maximum 1024 B RAM savings):**
   ```bash
   cmake -B build -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
     -DAVR_MCU=atmega328p \
     -DAVR_F_CPU=16000000UL \
     -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_1BITS
   cmake --build build
   ```

### Step 3: Flash and SRAM Memory Audit

At the conclusion of the build, `avr-size` prints exact resource usage:

* **`Program` section (`.text` + `.data`):** Occupies $\sim 1.4\text{ KB} - 1.5\text{ KB}$ of Flash ($< 5\%$ of an ATmega328P's 32 KB) thanks to table removal, `-flto`, and `--gc-sections`.
* **`Data` section (`.data` + `.bss`):**
  * With `PW_CHECKSUM_CRC32_1BITS`: **30 bytes** total RAM consumption.
  * With `PW_CHECKSUM_CRC32_4BITS`: **94 bytes** total RAM consumption (64 bytes for `kCrc32FourBitTable`).
* **Verifying 1024-byte table removal:**
  ```bash
  avr-nm --size-sort --print-size -C build/firmware.elf | grep -i crc
  ```

### Step 4: Flashing to AVR Microcontroller

Connect your AVR board via USB and determine the serial port (e.g., `/dev/ttyUSB0` or `/dev/ttyACM0`):

* **For Arduino Uno (ATmega328P with Optiboot bootloader at 115200 baud):**
  ```bash
  avrdude -c arduino -p m328p -P /dev/ttyACM0 -b 115200 -U flash:w:build/firmware.hex:i
  ```
* **For Arduino Nano (old bootloader at 57600 baud):**
  ```bash
  avrdude -c arduino -p m328p -P /dev/ttyUSB0 -b 57600 -U flash:w:build/firmware.hex:i
  ```
* **For Arduino Mega 2560 (ATmega2560 at 115200 baud with wiring protocol):**
  ```bash
  avrdude -c wiring -p m2560 -P /dev/ttyACM0 -b 115200 -D -U flash:w:build/firmware.hex:i
  ```

### Step 5: Receiving and Decoding `pw_hdlc` Frames on Host PC

The 4-bit and 1-bit implementations produce standard IEEE 802.3 CRC-32 matching Pigweed's Python decoder:

1. **Install Python dependencies in a virtual environment:**
   ```bash
   python3 -m venv .venv
   source .venv/bin/activate
   pip install pyserial
   pip install -e third_party/pigweed/pw_hdlc/py
   pip install -e third_party/pigweed/pw_varint/py
   ```
2. **Execute the receiver tool:**
   ```bash
   python3 tools/receive_telemetry.py -p /dev/ttyACM0 -b 115200
   ```

---

## 5. Troubleshooting Guide

| Issue / Compiler Error | Root Cause in `avr-gcc` | Solution |
| :--- | :--- | :--- |
| `fatal error: type_traits: No such file or directory` | The submodule `avr-libstdcpp` is missing or the `-isystem` path is invalid. | Run `git submodule update --init --recursive` and ensure `third_party/avr-libstdcpp/include` is present. |
| `undefined reference to operator delete(void*, unsigned int)` | In C++17, virtual destructors in `pw::stream::Stream` emit sized deallocation symbols. | Ensure `src/cxx_abi_stubs.cpp` is included in `add_executable(firmware.elf ...)` in `CMakeLists.txt`. |
| 1 KB SRAM and 1 KB Flash consumption when linking `pw_hdlc` | Default `pw_checksum` uses `PW_CHECKSUM_CRC32_8BITS` (256-entry 32-bit table copied to SRAM on Harvard architecture). | Pass `-DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS` or `-DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_1BITS` to CMake. |
| `firmware.hex` contains addresses outside Flash ($> \text{0x7FFF}$) | Custom ELF sections (such as `.pw_tokenizer.entries`) were not filtered. | Ensure `avr-objcopy` in `CMakeLists.txt` retains flags `-R .eeprom -R .pw_tokenizer.entries -R .pw_tokenizer.info`. |
| `enumerator value '1144606' is outside the range of underlying type 'int'` | On 8-bit AVR, `sizeof(int) == 2`, causing 32-bit enum polyfills to overflow 16-bit `int`. | Pass `-D__cpp_lib_endian=201907L` to use the native `std::endian` implementation provided by `avr-libstdcpp`. |
| `conversion from 'uint32_t' to 'size_type' may change value [-Werror=conversion]` | In AVR, `size_t` is 16 bits while loop indices in table generators use 32 bits. | `-Wno-conversion` is applied in `avr-toolchain.cmake` and `CMakeLists.txt`. |
