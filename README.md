# Pigweed port for 8-bit AVR

A CMake-based port of selected upstream Pigweed modules for bare-metal AVR MCUs, including ATmega328P for constrained probes and ATmega2560 as the minimum supported application target.

The port uses upstream **`pw_rpc`** and **`pw_transfer`**. The former custom `pw_rpc_avr` dispatcher has been removed; there is no parallel or fallback RPC implementation.

## Architecture

- **Transport:** upstream `pw_hdlc` over a bounded UART `pw::stream` adapter.
- **RPC:** upstream `pw::rpc::Server`, `pw::rpc::Client`, and raw streaming APIs.
- **Transfer:** upstream `pw_transfer::TransferService` and `pw_transfer::Client`, adapted for cooperative bare-metal execution.
- **AVR backends:** local implementations only where hardware-specific behavior is required, including clock, logging, assertions, crypto, and UART.
- **Simulation:** a `pw_emu` launcher/connector backed by `simavr` and a PTY UART.
- **Build:** CMake + `avr-gcc`; Pigweed and the AVR C++ standard-library support are pinned submodules.

The Pigweed revision used by the current RPC/transfer work is:

`55873baa1b37eaf7d0a830515a0846e3fc3a79e2`

## Build for the minimum supported application target (ATmega2560)

Initialize all submodules:

```sh
git submodule update --init --recursive
```

Configure and build:

```sh
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
  -DAVR_MCU=atmega328p \
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
simavr -m atmega328p build/pw_rpc_upstream_probe.elf
```

The firmware emits `PW_RPC_PROBE_PASS` only after the runtime assertions succeed.

## Cooperative `pw_transfer`

AVR cannot run Pigweed's blocking transfer-thread loop. The port applies an idempotent patch to the exact pinned Pigweed source and exposes a bounded cooperative poll API:

```cpp
std::array<std::byte, 128> chunk_storage{};
std::array<std::byte, 128> encode_storage{};

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

The full read/write transfer probe is built separately for ATmega2560:

```sh
cmake -B build-transfer -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake \
  -DAVR_MCU=atmega2560 \
  -DAVR_F_CPU=16000000UL \
  -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS

cmake --build build-transfer --target pw_transfer_upstream_probe.elf
simavr -m atmega2560 build-transfer/pw_transfer_upstream_probe.elf
```

The probe exercises read/write transfers, cancellation, recovery after a dropped response, retry exhaustion/deadline handling, and sequential client/server role reversal on the same two transfer-thread objects.

## Arduino library export

The exporter packages upstream RPC client/server sources and cooperative transfer sources and generated protocol headers:

```sh
python3 tools/export_arduino_library.py
```

Run the CMake configure/build steps above first, including the ATmega2560 transfer probe, so the generated `pw_transfer` PWPB and raw-RPC headers exist. The exporter fails if the cooperative source patch or generated headers are missing.

The exported umbrella header is `PigweedAVR.h`. It exposes upstream RPC and cooperative transfer headers and sets the AVR cooperative transfer configuration.

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
