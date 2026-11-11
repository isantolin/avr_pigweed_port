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

Pass `--pause` to create the PTY and wait without advancing the MCU. Send
`SIGUSR2` to the runner PID to resume, `SIGUSR1` to reset the MCU, and
`SIGINT`/`SIGTERM` to stop it. Reset discards pending UART TX bytes.


The runner prints the PTY slave path on stdout. Connect a serial client to that path using the firmware's configured baud rate and framing. Diagnostics are written to stderr.

## pw_emu integration

The repository registers a `simavr` emulator in the root `pigweed.json` and
provides a Python `Launcher`/`Connector` package at
`tools/pw_emu_avr/python/pw_emu_avr`.

Install the adapter and make the pinned Pigweed frontend importable:

```sh
python3 -m pip install -e third_party/pigweed/pw_emu/py
python3 -m pip install -e tools/pw_emu_avr
```

Build the runner, then launch an ELF through the normal frontend:

```sh
cmake -S tools/pw_emu_avr -B build/pw_emu_avr
cmake --build build/pw_emu_avr
pw emu start --file build/test_runner.elf --pause atmega328p
pw emu term uart0
```

The configured targets are `atmega328p` and `atmega2560`. The launcher
publishes the runner's PTY path through an instance metadata file and registers
it as the `uart0` channel. The connector maps `pw emu reset` and
`pw emu resume` to `SIGUSR1` and `SIGUSR2`.

## Scope and limitations

The backend currently exposes one selected UART (USART0 by default), loads ELF
firmware, and supports start, pause, resume, reset, stop, and PTY stream access.
It does not yet implement GDB, multiple UARTs, simulation-time pacing, or a
simavr object-property model. Those operations are intentionally not advertised
by the connector. All emulation code and dependencies remain host-side; the AVR
firmware has no `pw_emu` dependency.
