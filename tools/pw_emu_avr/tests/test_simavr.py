"""Unit and simavr integration tests for the Pigweed pw_emu AVR backend."""

from __future__ import annotations

import copy
import json
import os
import pty
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any

from pw_emu.core import (
    AlreadyRunning,
    ConfigError,
    Connector,
    InvalidChannelName,
    InvalidChannelType,
    InvalidProperty,
    InvalidPropertyPath,
    InvalidTarget,
    RunError,
    WrongEmulator,
)
from pw_emu.frontend import Emulator, TemporaryEmulator
from pw_emu_avr import (
    SimavrConnector,
    SimavrLauncher,
    SimavrMonitorError,
    scan_return_addresses,
    symbolize_diagnostics,
)
from serial import SerialException


ROOT = Path(__file__).resolve().parents[3]
CONFIG = ROOT / 'pigweed.json'
RUNNER_BIN = ROOT / 'build/pw_emu_avr/simavr_pty_runner'
FIRMWARE_328P = ROOT / 'build/pw_emu_avr/pw_emu_test_firmware.elf'
FIRMWARE_2560 = ROOT / 'build/pw_emu_avr/pw_emu_test_firmware_mega2560.elf'


def _write_temp_config(directory: Path, pw_emu_cfg: dict[str, Any]) -> Path:
    cfg_path = directory / 'pigweed.json'
    cfg_path.write_text(
        json.dumps({'pw': {'pw_emu': pw_emu_cfg}}, indent=2),
        encoding='utf-8',
    )
    return cfg_path


def _base_config(
    *,
    target_name: str = 'atmega328p',
    mcu: str = 'atmega328p',
    uarts: dict[str, Any] | None = None,
    gdb_cfg: dict[str, Any] | None = None,
    emu_channels: dict[str, Any] | None = None,
    emu_args: list[str] | None = None,
    target_args: list[str] | None = None,
) -> dict[str, Any]:
    simavr_target: dict[str, Any] = {
        'mcu': mcu,
        'frequency': 16000000,
        'channels': {
            'uarts': uarts if uarts is not None else {'uart0': {'id': '0'}},
        },
    }
    if gdb_cfg is not None:
        simavr_target['channels']['gdb'] = gdb_cfg
    if target_args is not None:
        simavr_target['args'] = target_args

    simavr_emu: dict[str, Any] = {
        'executable': str(RUNNER_BIN),
        'channels': emu_channels if emu_channels is not None else {'type': 'pty'},
    }
    if emu_args is not None:
        simavr_emu['args'] = emu_args

    return {
        'emulators': {
            'simavr': {
                'launcher': 'pw_emu_avr.simavr.SimavrLauncher',
                'connector': 'pw_emu_avr.simavr.SimavrConnector',
            }
        },
        'simavr': simavr_emu,
        'targets': {
            target_name: {
                'gdb': ['avr-gdb', '-ex', 'set remote memory-map-packet off'],
                'simavr': simavr_target,
            }
        },
    }


def _read_until(stream: Any, marker: bytes, *, timeout: float = 5.0, buf: bytearray | None = None) -> bytearray:
    output = buf if buf is not None else bytearray()
    deadline = time.monotonic() + timeout
    while marker not in output and time.monotonic() < deadline:
        try:
            chunk = stream.read(128)
        except (OSError, SerialException, socket.timeout):
            chunk = b''
        if chunk:
            output.extend(chunk)
        else:
            time.sleep(0.01)
    return output


def _gdb_rsp_exchange(host: str, port: int, packet: bytes) -> bytes:
    """Perform a minimal GDB Remote Serial Protocol packet exchange over TCP."""
    with socket.create_connection((host, port), timeout=3.0) as sock:
        sock.settimeout(3.0)
        sock.sendall(packet)
        data = bytearray()
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(1024)
            except socket.timeout:
                break
            if not chunk:
                break
            data.extend(chunk)
            if b'#' in data:
                hash_idx = data.index(b'#')
                if len(data) >= hash_idx + 3:
                    break
        sock.sendall(b'+')
        return bytes(data)


class SimavrUnitTest(unittest.TestCase):
    """Pure unit tests that do not require launching simavr."""

    def test_launcher_builds_command_for_channels_args_debug_and_legacy_uart(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            firmware = workdir / 'firmware.elf'
            firmware.write_bytes(b'ELF')

            cfg_path = _write_temp_config(
                workdir,
                _base_config(
                    target_name='mega',
                    mcu='atmega2560',
                    uarts={'uart0': {'id': '0', 'type': 'pty'}, 'uart1': {'id': '1', 'type': 'tcp'}},
                    emu_args=['--vcc', '5000'],
                    target_args=['--cycles-per-uart-byte', '1400'],
                ),
            )
            launcher = SimavrLauncher(cfg_path)
            launcher._wdir = workdir
            launcher._config.set_target('mega')
            command = launcher._pre_start(
                target='mega',
                file=firmware,
                pause=True,
                debug=True,
                args='--aref 3300',
            )

            self.assertIn('--mcu', command)
            self.assertIn('atmega2560', command)
            self.assertIn('--pause', command)
            self.assertIn('--log-level', command)
            self.assertIn('debug', command)
            self.assertIn('--channels-file', command)
            self.assertIn('--diagnostics-file', command)
            self.assertIn('--monitor-port', command)
            self.assertIn('--gdb-port', command)
            self.assertIn('uart0:0:pty', command)
            self.assertIn('uart1:1:tcp', command)
            self.assertIn('--vcc', command)
            self.assertIn('--cycles-per-uart-byte', command)
            self.assertIn('--aref', command)

    def test_launcher_supports_legacy_single_uart_and_no_firmware_with_gdb(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            cfg_dict = _base_config()
            del cfg_dict['targets']['atmega328p']['simavr']['channels']['uarts']
            cfg_dict['targets']['atmega328p']['simavr']['uart'] = '1'
            cfg_path = _write_temp_config(workdir, cfg_dict)

            launcher = SimavrLauncher(cfg_path)
            launcher._wdir = workdir
            launcher._config.set_target('atmega328p')
            cmd = launcher._pre_start(target='atmega328p', file=None, pause=True)
            self.assertIn('uart1:1:pty', cmd)
            self.assertNotIn('--firmware', cmd)

            with self.assertRaises(RunError):
                launcher._pre_start(target='atmega328p', file=None, pause=False)

    def test_launcher_rejects_invalid_channel_type(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            firmware = workdir / 'firmware.elf'
            firmware.write_bytes(b'ELF')
            cfg_path = _write_temp_config(
                workdir,
                _base_config(uarts={'uart0': {'id': '0', 'type': 'invalid'}}),
            )
            launcher = SimavrLauncher(cfg_path)
            launcher._wdir = workdir
            launcher._config.set_target('atmega328p')
            with self.assertRaises(InvalidChannelType):
                launcher._pre_start(target='atmega328p', file=firmware)

    def test_launcher_post_start_publishes_channels_and_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            launcher = SimavrLauncher(CONFIG)
            launcher._wdir = workdir
            launcher._config.set_target('atmega328p')

            # Spin up a tiny loopback responder for the monitor readiness probe.
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.bind(('127.0.0.1', 0))
            srv.listen(1)
            mon_port = srv.getsockname()[1]

            master, slave = pty.openpty()
            try:
                pty_path = os.ttyname(slave)
                (workdir / 'channels.json').write_text(
                    json.dumps(
                        {
                            'pid': os.getpid(),
                            'monitor': {'type': 'tcp', 'host': '127.0.0.1', 'port': mon_port},
                            'gdb': {'type': 'tcp', 'host': '127.0.0.1', 'port': 1234},
                            'uarts': {
                                'uart0': {'id': '0', 'type': 'pty', 'path': pty_path},
                                'uart1': {'id': '1', 'type': 'tcp', 'host': '127.0.0.1', 'port': 4321},
                            },
                        }
                    ),
                    encoding='utf-8',
                )
                launcher._handles.add_proc('simavr', os.getpid())

                import threading

                def _serve_one() -> None:
                    conn, _ = srv.accept()
                    with conn:
                        conn.recv(256)
                        conn.sendall(b'{"ok":true,"result":{"state":"stopped"}}\n')

                t = threading.Thread(target=_serve_one, daemon=True)
                t.start()
                launcher._post_start()
                t.join(timeout=2.0)

                self.assertEqual(launcher._handles.channels['uart0'].path, pty_path)
                self.assertTrue((workdir / 'uart0').is_symlink())
                self.assertEqual(launcher._handles.channels['uart1'].port, 4321)
                self.assertEqual(launcher._handles.channels['monitor'].port, mon_port)
                self.assertEqual(launcher._handles.channels['gdb'].port, 1234)
            finally:
                srv.close()
                os.close(master)
                os.close(slave)

    def test_connector_rejects_wrong_emulator(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            (workdir / 'handles.json').write_text(
                json.dumps(
                    {
                        'emu': 'qemu',
                        'config': str(CONFIG),
                        'target': 'atmega328p',
                        'gdb_cmd': [],
                        'channels': {},
                        'procs': {},
                    }
                ),
                encoding='utf-8',
            )
            with self.assertRaises(WrongEmulator):
                SimavrConnector(workdir)

    def test_diagnostics_helpers_handle_2byte_3byte_and_uninitialized_sp(self) -> None:
        # 2-byte word address 0x0050 -> byte address 0x00A0
        self.assertEqual(scan_return_addresses(bytes([0x00, 0x50]), 2, 0x1000), [0x00A0])
        # 3-byte word address 0x010050 -> byte address 0x0200A0 (ATmega2560)
        self.assertEqual(scan_return_addresses(bytes([0x01, 0x00, 0x50]), 3, 0x40000), [0x0200A0])

        with tempfile.TemporaryDirectory() as directory:
            fake_elf = Path(directory) / 'missing.elf'
            report = symbolize_diagnostics(
                fake_elf,
                {
                    'min_sp_valid': False,
                    'min_sp': None,
                    'ramend': 0x08FF,
                    'peak_pc': 0,
                    'address_size': 2,
                    'peak_stack_hex': '',
                    'crashed': False,
                },
            )
            self.assertFalse(report['min_sp_valid'])
            self.assertIsNone(report['min_sp'])
            self.assertIsNone(report['stack_used_bytes'])
            self.assertFalse(report['crashed'])


@unittest.skipUnless(
    RUNNER_BIN.is_file() and FIRMWARE_328P.is_file() and FIRMWARE_2560.is_file(),
    'host simavr_pty_runner and AVR test firmwares must be built',
)
class SimavrIntegrationTest(unittest.TestCase):
    """End-to-end integration tests exercising real simavr through pw_emu.frontend.Emulator."""

    def test_emulator_pty_lifecycle_pause_cont_reset_and_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wdir = Path(directory) / 'emu'
            emu = Emulator(wdir, CONFIG)
            emu.start('atmega328p', file=FIRMWARE_328P, pause=True)
            try:
                self.assertTrue(emu.running())
                self.assertTrue((wdir / 'simavr.pid').is_file())
                self.assertTrue((wdir / 'handles.json').is_file())
                self.assertTrue((wdir / 'uart0').is_symlink())
                self.assertEqual(emu.get_channel_type('uart0'), 'pty')
                self.assertEqual(emu.get_channel_type('monitor'), 'tcp')
                self.assertEqual(emu.get_channel_type('gdb'), 'tcp')
                self.assertTrue(os.path.exists(emu.get_channel_path('uart0')))
                self.assertIn('uart0', list(emu.get_channels()))

                with self.assertRaises(AlreadyRunning):
                    emu.start('atmega328p', file=FIRMWARE_328P)
                with self.assertRaises(InvalidChannelName):
                    emu.get_channel_type('nonexistent')
                with self.assertRaises(InvalidChannelType):
                    emu.get_channel_addr('uart0')

                # While paused, no UART output should arrive.
                with emu.get_channel_stream('uart0', timeout=0.3) as stream:
                    self.assertEqual(stream.read(64), b'')

                    emu.cont()
                    out = _read_until(stream, b'PW_EMU_UART_READY')
                    self.assertIn(b'PW_EMU_UART_READY', out)

                    # Write directly via the pw_emu channel stream.
                    stream.write(b'G')
                    out = _read_until(stream, b'PW_EMU_UART_PASS', buf=out)
                    self.assertIn(b'PW_EMU_UART_PASS', out)

                    # Pause via SimavrConnector extension and confirm cycle stops advancing.
                    conn = Connector.get(wdir)
                    self.assertIsInstance(conn, SimavrConnector)
                    assert isinstance(conn, SimavrConnector)
                    conn.pause()
                    c1 = emu.get_property('/cpu', 'cycle')
                    time.sleep(0.05)
                    c2 = emu.get_property('/cpu', 'cycle')
                    self.assertEqual(c1, c2)
                    self.assertTrue(emu.get_property('/cpu', 'paused'))

                    stepped = conn.step(10)
                    self.assertGreater(stepped['cycle'], c2)
                    self.assertEqual(stepped['state'], 'stopped')

                    # Synchronous reset while paused keeps MCU paused and resets min_sp.
                    emu.reset()
                    self.assertFalse(emu.get_property('/stack', 'min_sp_valid'))
                    self.assertIsNone(emu.get_property('/stack', 'min_sp'))

                    emu.cont()
                    out2 = _read_until(stream, b'PW_EMU_UART_READY')
                    self.assertIn(b'PW_EMU_UART_READY', out2)
                    stream.write(b'G')
                    out2 = _read_until(stream, b'PW_EMU_UART_PASS', buf=out2)
                    self.assertIn(b'PW_EMU_UART_PASS', out2)
                    self.assertTrue(emu.get_property('/stack', 'min_sp_valid'))
                    self.assertLessEqual(
                        emu.get_property('/stack', 'min_sp'),
                        emu.get_property('/stack', 'ramend'),
                    )
            finally:
                emu.stop()

            self.assertFalse(emu.running())
            self.assertFalse((wdir / 'simavr.pid').exists())
            self.assertFalse((wdir / 'handles.json').exists())
            self.assertFalse((wdir / 'channels.json').exists())
            self.assertFalse((wdir / 'uart0').exists())
            self.assertTrue((wdir / 'diagnostics.json').is_file())
            diag = json.loads((wdir / 'diagnostics.json').read_text(encoding='utf-8'))
            self.assertTrue(diag['min_sp_valid'])
            self.assertFalse(diag['crashed'])
            sym = symbolize_diagnostics(FIRMWARE_328P, diag)
            self.assertGreater(sym['stack_used_bytes'], 0)
            self.assertFalse(sym['stack_overflowed_into_bss'])

    def test_invalid_target_and_temporary_emulator_tcp_channel(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cfg_path = _write_temp_config(
                Path(directory),
                _base_config(uarts={'uart0': {'id': '0', 'type': 'tcp'}}),
            )
            with self.assertRaises(ConfigError):
                emu_bad = Emulator(Path(directory) / 'bad', cfg_path)
                emu_bad.start('unknown-target', file=FIRMWARE_328P)
            with self.assertRaises(InvalidTarget):
                SimavrLauncher(cfg_path).start(Path(directory) / 'bad2', 'unknown-target', file=FIRMWARE_328P)

            with TemporaryEmulator(config_path=cfg_path) as emu:
                emu.start('atmega328p', file=FIRMWARE_328P, pause=True)
                self.assertEqual(emu.get_channel_type('uart0'), 'tcp')
                host, port = emu.get_channel_addr('uart0')
                self.assertEqual(host, '127.0.0.1')
                self.assertGreater(port, 0)
                with self.assertRaises(InvalidChannelType):
                    emu.get_channel_path('uart0')

                with emu.get_channel_stream('uart0', timeout=1.0) as stream:
                    emu.cont()
                    out = _read_until(stream, b'PW_EMU_UART_READY')
                    self.assertIn(b'PW_EMU_UART_READY', out)
                    stream.write(b'G')
                    out = _read_until(stream, b'PW_EMU_UART_PASS', buf=out)
                    self.assertIn(b'PW_EMU_UART_PASS', out)

    def test_atmega2560_multi_uart_mixed_pty_and_tcp(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cfg_path = _write_temp_config(
                Path(directory),
                _base_config(
                    target_name='atmega2560',
                    mcu='atmega2560',
                    uarts={
                        'uart0': {'id': '0', 'type': 'pty'},
                        'uart1': {'id': '1', 'type': 'tcp'},
                    },
                ),
            )
            with TemporaryEmulator(config_path=cfg_path) as emu:
                emu.start('atmega2560', file=FIRMWARE_2560, pause=True)
                self.assertEqual(emu.get_channel_type('uart0'), 'pty')
                self.assertEqual(emu.get_channel_type('uart1'), 'tcp')
                self.assertEqual(emu.get_property('/cpu', 'mcu'), 'atmega2560')
                self.assertEqual(emu.get_property('/cpu', 'address_size'), 3)
                self.assertEqual(sorted(emu.list_properties('/uart')), ['uart0', 'uart1'])

                with (
                    emu.get_channel_stream('uart0', timeout=1.0) as s0,
                    emu.get_channel_stream('uart1', timeout=1.0) as s1,
                ):
                    emu.cont()
                    out0 = _read_until(s0, b'PW_EMU_UART0_READY')
                    out1 = _read_until(s1, b'PW_EMU_UART1_READY')
                    self.assertIn(b'PW_EMU_UART0_READY', out0)
                    self.assertIn(b'PW_EMU_UART1_READY', out1)

                    s0.write(b'0')
                    s1.write(b'1')
                    out0 = _read_until(s0, b'PW_EMU_UART0_PASS', buf=out0)
                    out1 = _read_until(s1, b'PW_EMU_UART1_PASS', buf=out1)
                    self.assertIn(b'PW_EMU_UART0_PASS', out0)
                    self.assertIn(b'PW_EMU_UART1_PASS', out1)

                self.assertGreater(emu.get_property('/uart/uart0', 'tx_bytes'), 0)
                self.assertGreater(emu.get_property('/uart/uart1', 'tx_bytes'), 0)
                self.assertEqual(emu.get_property('/uart/uart0', 'rx_dropped'), 0)
                self.assertEqual(emu.get_property('/uart/uart1', 'rx_dropped'), 0)

    def test_properties_cpu_memory_gpio_eeprom_adc_and_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cfg_path = _write_temp_config(
                Path(directory),
                _base_config(uarts={'uart0': {'id': '0', 'type': 'tcp'}}),
            )
            with TemporaryEmulator(config_path=cfg_path) as emu:
                emu.start('atmega328p', file=FIRMWARE_328P, pause=True)

                self.assertIn('cpu', emu.list_properties('/'))
                self.assertIn('pc', emu.list_properties('/cpu'))
                self.assertIn('portB', emu.list_properties('/gpio'))
                self.assertIn('adc0', emu.list_properties('/adc'))
                self.assertEqual(emu.get_property('/cpu', 'mcu'), 'atmega328p')
                self.assertEqual(emu.get_property('/cpu', 'frequency'), 16000000)

                # Invalid paths and properties must raise canonical pw_emu exceptions.
                with self.assertRaises(InvalidPropertyPath):
                    emu.list_properties('/does/not/exist')
                with self.assertRaises(InvalidPropertyPath):
                    emu.get_property('/does/not/exist', 'foo')
                with self.assertRaises(InvalidProperty):
                    emu.get_property('/cpu', 'no_such_prop')
                with self.assertRaises(InvalidProperty):
                    emu.set_property('/cpu', 'no_such_prop', 1)
                with self.assertRaises(SimavrMonitorError):
                    emu.set_property('/flash', '0x0000', 0xFF)

                # Register, SRAM, IO, Flash, ADC read/write checks.
                emu.set_property('/cpu', 'r5', 0xA5)
                self.assertEqual(emu.get_property('/cpu', 'r5'), 0xA5)
                emu.set_property('/cpu', 'vcc', 3300)
                self.assertEqual(emu.get_property('/cpu', 'vcc'), 3300)
                emu.set_property('/sram', '0x0200', 0x5C)
                self.assertEqual(emu.get_property('/sram', '0x0200'), 0x5C)
                self.assertGreater(emu.get_property('/flash', 'size'), 0)
                self.assertIsInstance(emu.get_property('/flash', '0x0000'), int)
                self.assertGreater(emu.get_property('/io', 'ioend'), 0)
                emu.set_property('/adc', 'adc0', 2500)
                self.assertEqual(emu.get_property('/adc', 'adc0'), 2500)

                # EEPROM & GPIO hardware interaction observed by the AVR firmware!
                emu.set_property('/eeprom', '0x0000', 0x5A)
                self.assertEqual(emu.get_property('/eeprom', '0x0000'), 0x5A)

                with emu.get_channel_stream('uart0', timeout=1.0) as stream:
                    emu.cont()
                    out = _read_until(stream, b'PW_EMU_UART_READY')
                    self.assertIn(b'PW_EMU_UART_READY', out)

                    stream.write(b'E')
                    out = _read_until(stream, b'EE0=5A', buf=out)
                    self.assertIn(b'EE0=5A', out)

                    emu.set_property('/gpio/portB', 'pin', 0x3C)
                    stream.write(b'P')
                    out = _read_until(stream, b'PINB=3C', buf=out)
                    self.assertIn(b'PINB=3C', out)

    def test_paused_without_run_produces_safe_uninitialized_stack_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wdir = Path(directory) / 'emu'
            emu = Emulator(wdir, CONFIG)
            emu.start('atmega328p', file=FIRMWARE_328P, pause=True)
            self.assertFalse(emu.get_property('/stack', 'min_sp_valid'))
            self.assertIsNone(emu.get_property('/stack', 'min_sp'))
            emu.stop()

            diag = json.loads((wdir / 'diagnostics.json').read_text(encoding='utf-8'))
            self.assertFalse(diag['min_sp_valid'])
            self.assertIsNone(diag['min_sp'])
            sym = symbolize_diagnostics(FIRMWARE_328P, diag)
            self.assertFalse(sym['min_sp_valid'])
            self.assertIsNone(sym['stack_used_bytes'])

    def test_crash_diagnostics_without_gdb(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cfg_path = _write_temp_config(
                Path(directory),
                _base_config(
                    uarts={'uart0': {'id': '0', 'type': 'tcp'}},
                    gdb_cfg={'enabled': False},
                ),
            )
            wdir = Path(directory) / 'crash_emu'
            emu = Emulator(wdir, cfg_path)
            emu.start('atmega328p', file=FIRMWARE_328P, pause=True)
            try:
                with self.assertRaises(InvalidChannelName):
                    emu.get_channel_type('gdb')
                with emu.get_channel_stream('uart0', timeout=1.0) as stream:
                    emu.cont()
                    out = _read_until(stream, b'PW_EMU_UART_READY')
                    self.assertIn(b'PW_EMU_UART_READY', out)
                    stream.write(b'X')

                deadline = time.monotonic() + 5.0
                while emu.running() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertFalse(emu.running())
            finally:
                emu.stop()

            diag = json.loads((wdir / 'diagnostics.json').read_text(encoding='utf-8'))
            self.assertTrue(diag['crashed'])
            self.assertTrue(diag['min_sp_valid'])
            sym = symbolize_diagnostics(FIRMWARE_328P, diag)
            self.assertTrue(sym['crashed'])
            self.assertGreater(sym['stack_used_bytes'], 0)

    def test_gdb_server_rsp_and_optional_avr_gdb(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cfg_path = _write_temp_config(
                Path(directory),
                _base_config(uarts={'uart0': {'id': '0', 'type': 'tcp'}}),
            )
            # Start without firmware in pause=True + GDB mode (pw emu load flow).
            with TemporaryEmulator(config_path=cfg_path) as emu:
                emu.start('atmega328p', file=None, pause=True)
                self.assertEqual(
                    emu.get_gdb_cmd(),
                    ['avr-gdb', '-ex', 'set remote memory-map-packet off'],
                )
                remote = emu.get_gdb_remote()
                host, port_s = remote.rsplit(':', 1)
                port = int(port_s)
                self.assertEqual(host, '127.0.0.1')
                self.assertGreater(port, 0)

                # Exercise live libsimavr GDB RSP while paused (works even if
                # avr-gdb binary is not installed on the local machine).
                stop_reply = _gdb_rsp_exchange(host, port, b'$?#3f')
                self.assertTrue(
                    b'$T05' in stop_reply or b'$S05' in stop_reply,
                    f'unexpected GDB stop reply: {stop_reply!r}',
                )
                # Disconnecting before firmware is loaded must not crash an
                # unprogrammed MCU; detach ($D) must not terminate the runner.
                self.assertTrue(emu.running())
                self.assertFalse(emu.get_property('/stack', 'crashed'))
                regs_reply = _gdb_rsp_exchange(host, port, b'$g#67')
                self.assertIn(b'$', regs_reply)
                self.assertGreater(len(regs_reply), 64)
                # simavr 1.7 replies `$OK` (and sets cpu_Done); simavr 1.6 replies `$#00`.
                detach_reply = _gdb_rsp_exchange(host, port, b'$D#44')
                self.assertTrue(
                    b'$OK' in detach_reply or b'$#00' in detach_reply,
                    f'unexpected GDB detach reply: {detach_reply!r}',
                )
                time.sleep(0.05)
                self.assertTrue(emu.running())

                # When avr-gdb is installed (e.g. Ubuntu CI with gdb-avr), also
                # verify Emulator.run_gdb_cmds(['load']) and detach end-to-end!
                if shutil.which('avr-gdb'):
                    with emu.get_channel_stream('uart0', timeout=2.0) as stream:
                        res = emu.run_gdb_cmds(['load'], executable=str(FIRMWARE_328P), pause=True)
                        self.assertEqual(
                            res.returncode,
                            0,
                            f'avr-gdb load failed: stdout={res.stdout!r} stderr={res.stderr!r}',
                        )
                        # Confirm flash vector 0 was programmed (no longer erased 0xFF).
                        self.assertNotEqual(
                            emu.get_property('/flash', '0x0000'),
                            0xFF,
                            f'flash remained erased after avr-gdb load; stdout={res.stdout!r} stderr={res.stderr!r}',
                        )
                        emu.cont()
                        out = _read_until(stream, b'PW_EMU_UART_READY')
                        self.assertIn(
                            b'PW_EMU_UART_READY',
                            out,
                            f'UART READY missing after gdb load; state={emu.get_property("/cpu", "state")!r} '
                            f'pc={emu.get_property("/cpu", "pc")!r} crashed={emu.get_property("/stack", "crashed")!r}',
                        )
                        stream.write(b'G')
                        out = _read_until(stream, b'PW_EMU_UART_PASS', buf=out)
                        self.assertIn(b'PW_EMU_UART_PASS', out)

                    # Default pause=False exits avr-gdb normally; runner must stay alive.
                    info_res = emu.run_gdb_cmds(['info registers pc'])
                    self.assertEqual(info_res.returncode, 0, info_res.stderr.decode('utf-8', errors='replace'))
                    self.assertTrue(emu.running())

    def test_pw_emu_cli_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wdir = Path(directory) / 'cli_wdir'
            env = copy.deepcopy(os.environ)

            def _cli(*args: str) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [
                        sys.executable,
                        '-m',
                        'pw_emu',
                        '-C',
                        str(wdir),
                        '-c',
                        str(CONFIG),
                        *args,
                    ],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=15.0,
                )

            try:
                res = _cli('start', '--pause', '--file', str(FIRMWARE_328P), 'atmega328p')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

                res = _cli('prop-ls', '/cpu')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertIn('"mcu"', res.stdout)

                res = _cli('prop-get', '/cpu', 'mcu')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertEqual(res.stdout.strip(), 'atmega328p')

                res = _cli('prop-set', '/cpu', 'r7', '42')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

                res = _cli('prop-get', '/cpu', 'r7')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertEqual(res.stdout.strip(), '42')

                res = _cli('resume')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

                res = _cli('reset')
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            finally:
                stop_res = _cli('stop')
                self.assertEqual(stop_res.returncode, 0, stop_res.stdout + stop_res.stderr)
            self.assertFalse((wdir / 'simavr.pid').exists())


if __name__ == '__main__':
    unittest.main()
