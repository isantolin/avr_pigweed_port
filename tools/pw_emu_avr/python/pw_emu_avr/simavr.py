"""pw_emu launcher and connector for the host-side simavr PTY runner."""

from __future__ import annotations

import os
import shlex
import signal
import time
from pathlib import Path

from pw_emu.core import Connector, Launcher, RunError, WrongEmulator


class SimavrLauncher(Launcher):
    """Launches simavr_pty_runner and registers its UART PTY with pw_emu."""

    def __init__(self, config_path: Path | None = None) -> None:
        super().__init__('simavr', config_path)

    def _pre_start(
        self,
        target: str,
        file: Path | None = None,
        pause: bool = False,
        debug: bool = False,
        args: str | None = None,
    ) -> list[str]:
        del debug  # pw_emu uses debug to select foreground execution.
        executable = self._config.get_target_emu(['executable'])
        if not executable:
            executable = self._config.get_emu(['executable'], optional=False)

        mcu = self._config.get_target_emu(['mcu'], optional=False)
        frequency = self._config.get_target_emu(['frequency'], optional=False)
        uart = self._config.get_target_emu(['uart']) or '0'

        if file is None:
            firmware = self._config.get_target_emu(['firmware'])
            if not firmware:
                raise RunError('simavr', f'No firmware ELF supplied for target {target}')
            file = Path(firmware)
        if not file.is_file():
            raise RunError('simavr', f'Firmware ELF does not exist: {file}')

        command = [
            str(executable),
            '--firmware',
            str(file.resolve()),
            '--mcu',
            str(mcu),
            '--frequency',
            str(frequency),
            '--uart',
            str(uart),
            '--pty-file',
            str(self._path('uart0.pty')),
        ]
        if pause:
            command.append('--pause')
        if args:
            command.extend(shlex.split(args))
        return command

    def _post_start(self) -> None:
        pty_file = self._path('uart0.pty')
        deadline = time.monotonic() + 10.0
        while not pty_file.is_file():
            process = self._handles.procs.get(self._emu)
            if process is not None:
                try:
                    os.kill(process.pid, 0)
                except ProcessLookupError as err:
                    raise RunError('simavr', 'simavr exited before publishing its UART PTY') from err
            if time.monotonic() >= deadline:
                raise RunError('simavr', 'timed out waiting for UART PTY metadata')
            time.sleep(0.05)

        try:
            pty_path = pty_file.read_text(encoding='utf-8').strip()
        except OSError as err:
            raise RunError('simavr', f'cannot read UART PTY metadata: {err}') from err
        if not pty_path or not os.path.exists(pty_path):
            raise RunError('simavr', f'simavr published an invalid UART PTY: {pty_path!r}')
        self._handles.add_channel_pty('uart0', pty_path)

    def _get_connector(self, wdir: Path) -> Connector:
        return SimavrConnector(wdir)


class SimavrConnector(Connector):
    """Controls the runner process and exposes its UART PTY."""

    def __init__(self, wdir: Path) -> None:
        super().__init__(wdir)
        if self.get_emu() != 'simavr':
            raise WrongEmulator('simavr', self.get_emu())

    def _signal_runner(self, requested_signal: int) -> None:
        process = self._handles.procs.get('simavr')
        if process is None:
            raise RunError('simavr', 'runner process handle is missing')
        try:
            os.kill(process.pid, requested_signal)
        except ProcessLookupError as err:
            raise RunError('simavr', 'runner process is no longer running') from err

    def reset(self) -> None:
        self._signal_runner(signal.SIGUSR1)

    def cont(self) -> None:
        self._signal_runner(signal.SIGUSR2)

    def list_properties(self, path: str) -> list[object]:
        del path
        return []

    def set_property(self, path: str, prop: str, value: str) -> None:
        raise NotImplementedError(
            f'simavr has no runtime property model ({path}.{prop}={value})'
        )

    def get_property(self, path: str, prop: str) -> object:
        raise NotImplementedError(f'simavr has no runtime property model ({path}.{prop})')

    def stop(self) -> None:
        super().stop()
        try:
            (self._wdir / 'uart0.pty').unlink()
        except FileNotFoundError:
            pass
