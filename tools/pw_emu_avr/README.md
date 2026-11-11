# AVR simulator backend for pw_emu

This directory contains the host-side `libsimavr` runner used by the AVR `pw_emu` integration.

## Boundary

- Firmware remains a normal AVR ELF and is built by the existing AVR toolchain.
- `simavr_pty_runner` runs on the host and links to `libsimavr`.
- USART0 is exposed as a raw POSIX PTY. The PTY is a byte stream in both directions.
- No emulator code, threads, or host dependencies are linked into AVR firmware.

## Build

On Debian/Ubuntu:

```sh
sudo apt-get install libsimavr-dev cmake build-essential
cmake -S tools/pw_emu_avr -B build/pw_emu_avr
cmake --build build/pw_emu_avr
```

## Run

```sh
build/pw_emu_avr/simavr_pty_runner \
  --firmware build/test_runner.elf \
  --mcu atmega328p \
  --frequency 16000000
```

The runner prints the PTY slave path on stdout. Connect a serial client to that path using the firmware's configured baud rate and framing. Diagnostics are written to stderr.

## Current scope and limitations

The runner currently exposes one UART (USART0 by default), loads ELF firmware, and runs until the firmware exits or the process receives SIGINT/SIGTERM. It does not yet implement GDB, AVR reset control, multiple UARTs, simulation-time pacing, or a `pw_emu.core.Launcher`/`Connector` adapter. Those features must be added and tested before advertising a complete `pw_emu` backend.

`pw_emu` documents external emulator types through Python `Launcher` and `Connector` implementations in `pigweed.json`; it is not currently integrated as a native CMake feature. See https://pigweed.dev/pw_emu/guide.html.
