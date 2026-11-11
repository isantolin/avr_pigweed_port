"""Unit and simavr integration tests for the pw_emu adapter."""

from __future__ import annotations

import os
import pty
import shutil
import signal
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import call, patch

from pw_emu_avr.simavr import SimavrConnector, SimavrLauncher


ROOT = Path(__file__).resolve().parents[3]
CONFIG = ROOT / 'pigweed.json'


class SimavrAdapterTest(unittest.TestCase):
    def test_launcher_builds_command_and_publishes_uart_channel(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            firmware = workdir / 'firmware.elf'
            firmware.write_bytes(b'ELF')

            launcher = SimavrLauncher(CONFIG)
            launcher._wdir = workdir
            launcher._config.set_target('atmega328p')
            command = launcher._pre_start(
                target='atmega328p', file=firmware, pause=True
            )

            self.assertIn('--mcu', command)
            self.assertIn('atmega328p', command)
            self.assertIn('--pause', command)
            self.assertIn('--pty-file', command)
            self.assertEqual(command[command.index('--frequency') + 1], '16000000')

            master, slave = pty.openpty()
            try:
                pty_path = os.ttyname(slave)
                launcher._handles.add_proc('simavr', os.getpid())
                (workdir / 'uart0.pty').write_text(pty_path, encoding='utf-8')
                launcher._post_start()
                self.assertEqual(
                    launcher._handles.channels['uart0'].path, pty_path
                )
            finally:
                os.close(master)
                os.close(slave)

    def test_connector_reset_resume_and_uart_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workdir = Path(directory)
            launcher = SimavrLauncher(CONFIG)
            launcher._wdir = workdir
            launcher._config.set_target('atmega328p')
            launcher._handles.add_proc('simavr', os.getpid())
            launcher._handles.add_channel_pty('uart0', '/dev/pts/123')
            launcher._handles.save(workdir)

            connector = SimavrConnector(workdir)
            self.assertEqual(connector.get_channel_path('uart0'), '/dev/pts/123')
            with patch('pw_emu_avr.simavr.os.kill') as kill:
                connector.reset()
                connector.cont()
            self.assertEqual(
                kill.call_args_list,
                [call(os.getpid(), signal.SIGUSR1), call(os.getpid(), signal.SIGUSR2)],
            )

    @unittest.skipUnless(
        (ROOT / 'build/pw_emu_avr/simavr_pty_runner').is_file()
        and (ROOT / 'build/test_runner.elf').is_file(),
        'host runner and AVR test firmware must be built',
    )
    def test_pw_emu_launches_runner_and_reads_uart(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            launcher = SimavrLauncher(CONFIG)
            connector = launcher.start(
                Path(directory),
                'atmega328p',
                file=ROOT / 'build/test_runner.elf',
                pause=True,
            )
            stream = None
            try:
                stream = connector.get_channel_stream('uart0', timeout=1.0)
                connector.cont()
                output = bytearray()
                deadline = time.monotonic() + 20.0
                self.assertTrue(connector.running(), connector.get_logs())
                while b'[  PASSED  ]' not in output and time.monotonic() < deadline:
                    try:
                        chunk = stream.read(256)
                    except OSError:
                        # simavr may close the PTY after the firmware's main
                        # function reaches its terminal sleep/exit state.
                        break
                    if chunk:
                        output.extend(chunk)
                self.assertIn(
                    b'[  PASSED  ]',
                    output,
                    f'UART output missing; runner logs: {connector.get_logs()}',
                )
            finally:
                if stream is not None:
                    stream.close()
                connector.stop()


if __name__ == '__main__':
    unittest.main()
