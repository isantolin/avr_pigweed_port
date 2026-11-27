# Pigweed port for 8-bit AVR

A CMake-based port of selected upstream Pigweed modules for bare-metal AVR MCUs, including ATmega328P for constrained probes and ATmega2560 as the minimum supported application target.

The port uses upstream **`pw_rpc`** and **`pw_transfer`**. The former custom `pw_rpc_avr` dispatcher has been removed; there is no parallel or fallback RPC implementation.

## Architecture

- **Transport:** upstream `pw_hdlc` over a bounded UART `pw::stream` adapter.
- **RPC:** upstream `pw::rpc::Server`, `pw::rpc::Client`, and raw streaming APIs.
- **Transfer:** upstream `pw_transfer::TransferService` and `pw_transfer::Client`, adapted for cooperative bare-metal execution.
- **AVR backends:** local implementations only where hardware-specific behavior is required, including clock, logging, assertions, and UART. The standalone SHA-256 backend uses a 16-word rolling schedule; CI emits compiler-measured stack-usage metadata for it.
- **Simulation:** a `pw_emu` launcher/connector backed by `simavr` and a PTY UART.
- **Build:** CMake + `avr-gcc`; Pigweed and the AVR C++ standard-library support are pinned submodules. ATmega2560 is the minimum supported application target; smaller AVRs are used only for isolated component probes.

The Pigweed revision used by the current RPC/transfer work is:

`247492d08e49dc912bc60e855b57c542a84257df`

## Bazel host integration

The AVR port exposes its upstream-Pigweed peripheral adapters as Bzlmod targets.
The dependency graph is locked in the committed `MODULE.bazel.lock`; CI verifies
the lockfile with Bazel 9.3.3 using `--lockfile_mode=error`.

Run the host integration test with:

```sh
bazel test --lockfile_mode=error //tests/host:peripheral_backends_upstream_test --test_output=errors
```

This host test validates the adapter interfaces and is separate from the
ATmega2560 firmware build and the simavr runtime probes below.

## Build for the minimum supported application target (ATmega2560)

Initialize all submodules:

```sh
git submodule update --init --recursive
```

Configure and build:

```sh
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
  -DAVR_MCU=atmega2560 \
  -DAVR_F_CPU=16000000UL \
  -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS

cmake --build build --target all
cmake --build build --target run_tests
```

To use the smaller CRC implementation, configure with `-DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_1BITS`. CI builds both configurations.

## Upstream RPC validation

`pw_rpc_upstream_probe.elf` is an isolated diagnostic target; CI builds and runs it on ATmega1284P because it contains both RPC endpoints. It exercises unary request/response and bidirectional raw streaming through upstream `pw_rpc` and an HDLC encode/decode loopback.

Run it with:

```sh
simavr -m atmega1284p build-transfer/pw_rpc_upstream_probe.elf
```

The firmware emits `PW_RPC_PROBE_PASS` only after the runtime assertions succeed.

## Cooperative `pw_transfer`

AVR cannot run Pigweed's blocking transfer-thread loop. The port applies an idempotent patch to the exact pinned Pigweed source and exposes a bounded cooperative poll API:

```cpp
pw::byte chunk_storage[128]{};
pw::byte encode_storage[128]{};

pw::transfer::Thread<1, 1> transfer_thread{
    pw::ByteSpan(chunk_storage), pw::ByteSpan(encode_storage)};

pw::transfer::TransferService transfer_service(transfer_thread, 256);
pw::transfer::Client transfer_client(
    rpc_client, channel_id, transfer_thread, 256);

// Call regularly from the cooperative MCU loop.
(void)transfer_thread.Poll(2);
```

A single `Thread<1, 1>` can be reused for client and server transfers **sequentially**, not simultaneously. A role arbiter enforces this. The event queue and chunk storage are statically bounded; overflow and staging conflicts are returned as errors rather than silently dropping data.

Detailed design, constraints, and validation status: [Cooperative upstream pw_transfer on AVR](docs/pw_transfer_avr_cooperative.md).

### End-to-end transfer probe

The full read/write transfer probe is an isolated ATmega1284P component test (not an application target):

```sh
cmake -B build-transfer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
  -DAVR_MCU=atmega2560 \
  -DAVR_F_CPU=16000000UL \
  -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS

cmake --build build-transfer --target pw_transfer_upstream_probe.elf
simavr -m atmega1284p build-transfer/pw_transfer_upstream_probe.elf
```

The probe exercises read/write transfers, cancellation, recovery after a dropped response, retry exhaustion/deadline handling, and sequential client/server role reversal on the same two transfer-thread objects.

## Arduino library export

The exporter packages upstream RPC client/server sources and, when generated bindings are available, cooperative transfer sources and generated protocol headers:

```sh
python3 tools/export_arduino_library.py
```

Run the CMake configure/build steps above first, including the ATmega1284P transfer probe, so the generated `pw_transfer` PWPB and raw-RPC headers exist. Host-only and non-Mega exports may omit those generated bindings; the exporter warns when they are absent. `PigweedAVR.h` exposes the transfer API only when compiling for ATmega2560.

The exported umbrella header is `PigweedAVR.h`. It exposes upstream RPC on supported Arduino targets and adds cooperative transfer headers/configuration only for ATmega2560. Port-owned headers, sketches, and probes avoid STL containers (`std::array`, `std::vector`) and `PigweedAVR.h` intentionally omits `pw_containers/vector.h`. Because upstream Google Pigweed C++17 internals rely on freestanding `<new>`, `<array>`, and `<type_traits>`, Arduino CLI AVR builds must use a modern `avr-g++` (>= 8) with `-std=gnu++17 -D__cpp_lib_endian=201907L` and pass `-I<libraries>/PigweedAVR/gcc<ver>/include` in `compiler.cpp.extra_flags` so `avr-libstdcpp`'s freestanding C++17 `<new>` takes precedence over the Arduino AVR core's C++11 `cores/arduino/new` while allowing Arduino CLI to discover and compile `PigweedAVR/src/*.cpp`. The export does not bundle the Nanopb C runtime: applications that use `pw_rpc.nanopb` must provide Nanopb 0.4.9.3 separately. The Arduino Bridge installer supplies those `pb_*` sources in its application library, avoiding duplicate runtime symbols in `PigweedAVR`.

## `pw_emu` + simavr

The `tools/pw_emu_avr` directory contains the host-side simavr PTY runner and Pigweed `pw_emu` launcher/connector. It exposes UART as a PTY and supports pause, resume, reset, and process lifecycle handling.

See [tools/pw_emu_avr/README.md](tools/pw_emu_avr/README.md) for setup and usage. CI builds a minimal ATmega328P UART firmware and checks the runner's UART/lifecycle integration.

## Repository layout

- `pw_assert_avr/`, `pw_chrono_avr/`, `pw_crypto_avr/`, `pw_log_avr/`: hardware-specific Pigweed backends.
- `src/`: AVR UART stream, firmware entry point, and C++ ABI support.
- `tests/`: module tests and isolated upstream RPC/transfer probes.
- `tools/ci/`: pinned-source audits and idempotent Pigweed compatibility patches.
- `tools/export_arduino_library.py`: Arduino library packaging.
- `tools/pw_emu_avr/`: simavr runner and `pw_emu` integration.
- `third_party/pigweed/`: pinned upstream Pigweed submodule.

## Current validation boundary

The port's CI validates both CRC32 configurations and runs the full module test suite on the ATmega2560 minimum application target. Isolated upstream RPC and transfer runtime probes run on ATmega1284P; CI also checks the Arduino Mega 2560 export syntax and export contents.

Physical UART/HDLC interoperability on the Bridge hardware, complete Bridge firmware memory/stack measurements, and application-level migration of the Bridge's file operations remain separate integration gates. Do not treat the isolated probes as proof that the complete Bridge firmware is production-ready.
