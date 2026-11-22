# AVR simulator backend for `pw_emu`

This directory contains the host-side `libsimavr` runner and Pigweed `pw_emu`
adapter (`pw_emu_avr`) for AVR targets.

## Boundary

- Firmware remains a normal AVR ELF (or Intel HEX) built by the AVR toolchain.
- `simavr_pty_runner` runs strictly on the host and links to `libsimavr`.
- Up to four AVR USART channels (`0..3`, depending on MCU) can be exposed as
  POSIX PTYs or loopback TCP sockets.
- A synchronous loopback TCP `monitor` channel provides deterministic control
  (`reset`, `pause`, `cont`, `step`, `status`, `stack_stats`, `quit`) and a
  `libsimavr`-backed property model (`/cpu`, `/stack`, `/sram`, `/io`, `/flash`,
  `/eeprom`, `/gpio/port<A..L>`, `/adc`, `/uart/<name>`).
- Optional native `libsimavr` GDB RSP support (`avr_gdb_init`) exposes a `gdb`
  TCP channel for `pw emu gdb`, `pw emu gdb-cmds`, and `pw emu load`.
- No emulator code, threads, or host dependencies are linked into AVR firmware.

## Build

On Debian/Ubuntu:

```sh
sudo apt-get install libsimavr-dev cmake build-essential gdb-avr
cmake -S tools/pw_emu_avr -B build/pw_emu_avr
cmake --build build/pw_emu_avr
```

## Run standalone

```sh
build/pw_emu_avr/simavr_pty_runner \
  --firmware build/test_runner.elf \
  --mcu atmega2560 \
  --frequency 16000000 \
  --uart-channel uart0:0:pty \
  --uart-channel uart1:1:tcp \
  --channels-file /tmp/simavr-channels.json \
  --diagnostics-file /tmp/simavr-diagnostics.json \
  --monitor-port 0 \
  --gdb-port 1234
```

`--mcu`, `--frequency`, and at least one `--uart-channel NAME:ID:pty|tcp` are
required. Pass `--pause` to start halted before advancing the MCU. Channel
endpoints are published atomically via `--channels-file`, runtime control
(`reset`, `pause`, `cont`, `step`, `quit`, properties) is handled over the
synchronous TCP `monitor` channel, and `SIGINT`/`SIGTERM` trigger graceful
shutdown and atomic `--diagnostics-file` emission.

## `pw_emu` integration

The repository registers `simavr` in the root `pigweed.json` and provides the
Python package at `tools/pw_emu_avr/python/pw_emu_avr`.

Install the pinned Pigweed environment packages and the local adapter:

```sh
python3 -m pip install -e third_party/pigweed/pw_env_setup/py
python3 -m pip install -e third_party/pigweed/pw_cli/py
python3 -m pip install -e third_party/pigweed/pw_emu/py
python3 -m pip install --no-build-isolation --no-deps -e tools/pw_emu_avr
```

Launch an ELF through the standard `pw_emu` frontend or CLI:

```sh
pw emu start --file build/test_runner.elf --pause atmega2560
pw emu prop-get /cpu mcu
pw emu resume
pw emu term uart0
pw emu gdb -e build/test_runner.elf
pw emu stop
```

Configured targets in `tools/pw_emu_avr/targets.json`: `atmega328p`,
`atmega2560`, `atmega1284p`, and `atmega32u4`.

> Note on multi-emulator `pigweed.json` configurations: upstream
> `pw_emu.core.Config` merges top-level `emulators` shallowly. Projects using
> `simavr` alongside `qemu` and/or `renode` in a single config file should list
> all desired backends under `pw.pw_emu.emulators`.

## Diagnostics

`simavr_pty_runner` tracks the runtime AVR stack low-water mark (`min_sp`), peak
call context, and crash snapshots, exposing them live via `/stack` and writing
`diagnostics.json` atomically on exit. `pw_emu_avr.diagnostics.symbolize_diagnostics()`
enriches those records using `avr-nm` and `avr-addr2line`.
