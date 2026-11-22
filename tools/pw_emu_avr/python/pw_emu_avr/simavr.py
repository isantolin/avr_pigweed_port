"""Pigweed pw_emu launcher and connector for the host-side simavr runner."""

from __future__ import annotations

import io
import json
import os
import shlex
import shutil
import socket
import sys
import time
from pathlib import Path
from typing import Any

import psutil

from pw_emu.core import (
    ConfigError,
    Connector,
    Error,
    InvalidChannelType,
    InvalidProperty,
    InvalidPropertyPath,
    Launcher,
    RunError,
    WrongEmulator,
)


class SimavrMonitorError(Error):
    """Exception raised when the simavr JSON-lines monitor channel returns an error."""

    def __init__(self, message: str) -> None:
        super().__init__(message)


class SimavrMonitorClient:
    """Synchronous JSON-lines client for the simavr_pty_runner monitor channel."""

    def __init__(self, stream: io.RawIOBase) -> None:
        self._stream = stream

    def close(self) -> None:
        """Close the underlying monitor stream."""
        try:
            self._stream.close()
        except OSError:
            pass

    def request(self, cmd: str, **params: Any) -> Any:
        """Send a single monitor request and return the decoded result."""
        payload: dict[str, Any] = {'cmd': cmd, **params}
        wire = (json.dumps(payload, separators=(',', ':')) + '\n').encode('utf-8')
        try:
            self._stream.write(wire)
            raw_line = self._stream.readline()
        except OSError as err:
            raise RunError('simavr', f'monitor I/O failed during `{cmd}`: {err}') from err

        if not raw_line:
            raise RunError('simavr', f'monitor channel closed during `{cmd}`')

        try:
            response = json.loads(raw_line.decode('utf-8'))
        except (UnicodeDecodeError, json.JSONDecodeError) as err:
            raise SimavrMonitorError(f'malformed monitor response: {raw_line!r}') from err

        if not isinstance(response, dict):
            raise SimavrMonitorError(f'expected object monitor response, got {type(response).__name__}')

        if response.get('ok') is True:
            return response.get('result')

        code = str(response.get('error', 'runtime_error'))
        message = str(response.get('message', code))
        if code == 'invalid_path':
            raise InvalidPropertyPath(str(params.get('path', '')))
        if code == 'invalid_property':
            raise InvalidProperty(str(params.get('path', '')), str(params.get('property', '')))
        raise SimavrMonitorError(message)


class SimavrLauncher(Launcher):
    """Launches simavr_pty_runner and registers monitor, GDB, and UART channels."""

    def __init__(self, config_path: Path | None = None) -> None:
        super().__init__('simavr', config_path)
        self._configured_uarts: list[tuple[str, str, str]] = []
        self._gdb_port: int | None = None

    @staticmethod
    def _allocate_tcp_port() -> int:
        """Reserve an ephemeral loopback TCP port for libsimavr's GDB server."""
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(('127.0.0.1', 0))
            return int(sock.getsockname()[1])

    def _resolve_executable(self) -> str:
        raw = self._config.get_target_emu(['executable'])
        if not raw:
            raw = self._config.get_emu(['executable'], optional=False)
        exe = str(raw)
        candidate = Path(exe)
        if candidate.is_absolute():
            if candidate.is_file():
                return str(candidate)
            raise RunError('simavr', f'simavr runner executable does not exist: {candidate}')

        # Prefer paths relative to the config file directory when the entry
        # includes a directory component (e.g. "build/pw_emu_avr/simavr_pty_runner").
        if os.sep in exe or '/' in exe:
            config_relative = (Path(self._config.path).resolve().parent / candidate).resolve()
            if config_relative.is_file():
                return str(config_relative)
            cwd_relative = candidate.resolve()
            if cwd_relative.is_file():
                return str(cwd_relative)
            # Fall back to the configured string so unit tests inspecting _pre_start
            # without a compiled runner still observe the resolved command.
            return str(config_relative)

        found = shutil.which(exe)
        if found:
            return found
        return exe

    def _get_channel_type(
        self,
        chan_name: str,
        explicit_type: str | None = None,
        *,
        group: str = 'uarts',
        default: str = 'pty',
    ) -> str:
        chan_type: Any = explicit_type
        if chan_type is None:
            chan_type = self._config.get_target_emu(['channels', group, chan_name, 'type'])
        if chan_type is None:
            chan_type = self._config.get_target_emu(['channels', group, 'type'])
        if chan_type is None:
            chan_type = self._config.get_target_emu(['channels', 'type'])
        if chan_type is None:
            chan_type = self._config.get_emu(['channels', group, 'type'])
        if chan_type is None:
            chan_type = self._config.get_emu(['channels', 'type'])
        if chan_type is None:
            chan_type = default

        if not isinstance(chan_type, str) or chan_type not in ('pty', 'tcp'):
            raise InvalidChannelType(str(chan_type))
        if chan_type == 'pty' and sys.platform == 'win32':
            raise InvalidChannelType(chan_type)
        return chan_type

    def _configure_uarts(self) -> list[tuple[str, str, str]]:
        uarts_cfg = self._config.get_target_emu(['channels', 'uarts'], entry_type=dict)
        result: list[tuple[str, str, str]] = []
        if uarts_cfg:
            for name, cfg in uarts_cfg.items():
                if not isinstance(cfg, dict) or 'id' not in cfg:
                    raise ConfigError(
                        self._config.path,
                        f'simavr UART channel `{name}` is missing required `id` field',
                    )
                uart_id = str(cfg['id'])
                if len(uart_id) != 1 or uart_id not in '0123':
                    raise ConfigError(
                        self._config.path,
                        f'simavr UART channel `{name}` has invalid id `{uart_id}` (expected 0..3)',
                    )
                chan_type = self._get_channel_type(name, cfg.get('type'), group='uarts', default='pty')
                result.append((str(name), uart_id, chan_type))
            return result

        # Legacy single-UART configuration fallback (`uart: "0"`).
        legacy_uart = str(self._config.get_target_emu(['uart']) or '0')
        if len(legacy_uart) != 1 or legacy_uart not in '0123':
            raise ConfigError(
                self._config.path,
                f'simavr target has invalid uart `{legacy_uart}` (expected 0..3)',
            )
        chan_name = f'uart{legacy_uart}'
        chan_type = self._get_channel_type(chan_name, group='uarts', default='pty')
        result.append((chan_name, legacy_uart, chan_type))
        return result

    def _configure_gdb_port(self) -> int | None:
        target_gdb_cfg = self._config.get_target_emu(['channels', 'gdb'], entry_type=dict)
        emu_gdb_cfg = self._config.get_emu(['channels', 'gdb'], entry_type=dict)

        enabled: Any = target_gdb_cfg.get('enabled')
        if enabled is None:
            enabled = emu_gdb_cfg.get('enabled')
        if enabled is False:
            return None

        gdb_type: Any = target_gdb_cfg.get('type') or emu_gdb_cfg.get('type') or 'tcp'
        if gdb_type != 'tcp':
            raise InvalidChannelType(str(gdb_type))

        explicit_port: Any = target_gdb_cfg.get('port')
        if explicit_port is None:
            explicit_port = emu_gdb_cfg.get('port')
        if explicit_port is not None:
            if not isinstance(explicit_port, int) or explicit_port <= 0 or explicit_port > 65535:
                raise ConfigError(self._config.path, f'invalid simavr gdb port: {explicit_port!r}')
            return explicit_port
        return self._allocate_tcp_port()

    def _pre_start(
        self,
        target: str,
        file: Path | None = None,
        pause: bool = False,
        debug: bool = False,
        args: str | None = None,
    ) -> list[str]:
        # Remove stale metadata from any prior interrupted run in the same wdir.
        for stale in ('channels.json', 'diagnostics.json', 'uart0.pty'):
            try:
                self._path(stale).unlink()
            except FileNotFoundError:
                pass

        executable = self._resolve_executable()
        mcu = self._config.get_target_emu(['mcu'], optional=False)
        frequency = self._config.get_target_emu(['frequency'], optional=False)
        self._configured_uarts = self._configure_uarts()
        self._gdb_port = self._configure_gdb_port()

        if file is None:
            firmware = self._config.get_target_emu(['firmware'])
            if firmware:
                file = Path(firmware)
                if not file.is_absolute():
                    candidate = (Path(self._config.path).resolve().parent / file).resolve()
                    if candidate.is_file():
                        file = candidate
        if file is None:
            if not (pause and self._gdb_port is not None):
                raise RunError(
                    'simavr',
                    f'No firmware supplied for target {target} '
                    '(pass file= or start with pause=True and GDB enabled)',
                )
        elif not Path(file).is_file():
            raise RunError('simavr', f'Firmware image does not exist: {file}')

        command: list[str] = [
            executable,
            '--mcu',
            str(mcu),
            '--frequency',
            str(frequency),
            '--channels-file',
            str(self._path('channels.json')),
            '--diagnostics-file',
            str(self._path('diagnostics.json')),
            '--monitor-port',
            '0',
        ]
        if file is not None:
            command.extend(['--firmware', str(Path(file).resolve())])
        for name, uart_id, chan_type in self._configured_uarts:
            command.extend(['--uart-channel', f'{name}:{uart_id}:{chan_type}'])
        if self._gdb_port is not None:
            command.extend(['--gdb-port', str(self._gdb_port)])
        if pause:
            command.append('--pause')
        if debug:
            command.extend(['--log-level', 'debug'])

        command.extend(str(item) for item in self._config.get_emu(['args'], entry_type=list))
        command.extend(str(item) for item in self._config.get_target_emu(['args'], entry_type=list))
        if args:
            command.extend(shlex.split(args))
        return command

    @staticmethod
    def _is_pid_alive(pid: int) -> bool:
        try:
            proc = psutil.Process(pid)
            return proc.is_running() and proc.status() != psutil.STATUS_ZOMBIE
        except psutil.Error:
            return False

    def _register_pty_channel(self, name: str, pty_path: str) -> None:
        if not pty_path or not os.path.exists(pty_path):
            raise RunError('simavr', f'simavr published an invalid PTY for `{name}`: {pty_path!r}')
        self._handles.add_channel_pty(name, pty_path)
        link_path = self._path(name)
        if os.path.lexists(link_path):
            os.unlink(link_path)
        os.symlink(pty_path, link_path)

    def _post_start(self) -> None:
        channels_file = self._path('channels.json')
        deadline = time.monotonic() + 10.0
        while not channels_file.is_file():
            process = self._handles.procs.get(self._emu)
            if process is not None and not self._is_pid_alive(process.pid):
                raise RunError('simavr', 'simavr exited before publishing channels metadata')
            if time.monotonic() >= deadline:
                self._stop_procs()
                raise RunError('simavr', 'timed out waiting for simavr channels metadata')
            time.sleep(0.02)

        try:
            metadata = json.loads(channels_file.read_text(encoding='utf-8'))
        except (OSError, json.JSONDecodeError) as err:
            self._stop_procs()
            raise RunError('simavr', f'cannot parse channels metadata: {err}') from err

        if not isinstance(metadata, dict):
            self._stop_procs()
            raise RunError('simavr', 'channels metadata root must be a JSON object')

        monitor = metadata.get('monitor')
        if not isinstance(monitor, dict) or monitor.get('type') != 'tcp':
            self._stop_procs()
            raise RunError('simavr', 'simavr did not publish a TCP monitor channel')
        self._handles.add_channel_tcp('monitor', str(monitor['host']), int(monitor['port']))

        gdb = metadata.get('gdb')
        if isinstance(gdb, dict) and gdb.get('type') == 'tcp':
            self._handles.add_channel_tcp('gdb', str(gdb['host']), int(gdb['port']))

        uarts = metadata.get('uarts')
        if not isinstance(uarts, dict):
            self._stop_procs()
            raise RunError('simavr', 'simavr channels metadata missing `uarts` map')
        for name, info in uarts.items():
            if not isinstance(info, dict):
                self._stop_procs()
                raise RunError('simavr', f'malformed UART metadata for `{name}`')
            chan_type = info.get('type')
            if chan_type == 'pty':
                self._register_pty_channel(str(name), str(info.get('path', '')))
            elif chan_type == 'tcp':
                self._handles.add_channel_tcp(str(name), str(info['host']), int(info['port']))
            else:
                self._stop_procs()
                raise InvalidChannelType(str(chan_type))

        # Verify synchronous monitor readiness before returning the Connector.
        try:
            with socket.create_connection((str(monitor['host']), int(monitor['port'])), timeout=2.0) as sock:
                sock.settimeout(2.0)
                stream = sock.makefile('rwb', buffering=0)
                try:
                    SimavrMonitorClient(stream).request('status')
                finally:
                    stream.close()
        except (OSError, Error) as err:
            self._stop_procs()
            raise RunError('simavr', f'monitor handshake failed: {err}') from err

    def _get_connector(self, wdir: Path) -> Connector:
        return SimavrConnector(wdir)


class SimavrConnector(Connector):
    """Controls a running simavr instance over its synchronous TCP monitor channel."""

    def __init__(self, wdir: Path) -> None:
        super().__init__(wdir)
        if self.get_emu() != 'simavr':
            raise WrongEmulator('simavr', self.get_emu())
        self._monitor: SimavrMonitorClient | None = None

    def _m(self) -> SimavrMonitorClient:
        if self._monitor is None:
            self._monitor = SimavrMonitorClient(self.get_channel_stream('monitor', timeout=5.0))
        return self._monitor

    def _close_monitor(self) -> None:
        if self._monitor is not None:
            self._monitor.close()
            self._monitor = None

    def _monitor_cmd(self, cmd: str, **params: Any) -> Any:
        try:
            return self._m().request(cmd, **params)
        except RunError:
            # Retry once with a fresh socket if the previous idle connection was closed.
            self._close_monitor()
            if not self.running():
                raise
            return self._m().request(cmd, **params)

    def reset(self) -> None:
        self._monitor_cmd('reset')

    def cont(self) -> None:
        self._monitor_cmd('cont')

    def pause(self) -> None:
        """Pause AVR execution deterministically via the monitor channel."""
        self._monitor_cmd('pause')

    def step(self, cycles: int = 1) -> dict[str, Any]:
        """Advance AVR execution by ``cycles`` steps and remain paused."""
        return dict(self._monitor_cmd('step', cycles=cycles))

    def status(self) -> dict[str, Any]:
        """Return live execution state (state, paused, crashed, pc, sp, cycle)."""
        return dict(self._monitor_cmd('status'))

    def get_diagnostics(self) -> dict[str, Any] | None:
        """Return stack watermark and crash diagnostics (live or persisted post-exit)."""
        if self.running():
            try:
                return dict(self._monitor_cmd('stack_stats'))
            except (Error, OSError):
                pass
        diag_path = self._wdir / 'diagnostics.json'
        if not diag_path.is_file():
            return None
        try:
            data = json.loads(diag_path.read_text(encoding='utf-8'))
        except (OSError, json.JSONDecodeError):
            return None
        return data if isinstance(data, dict) else None

    def list_properties(self, path: str) -> list[Any]:
        return list(self._monitor_cmd('prop_list', path=path))

    def set_property(self, path: str, prop: str, value: Any) -> None:
        self._monitor_cmd('prop_set', path=path, property=prop, value=value)

    def get_property(self, path: str, prop: str) -> Any:
        return self._monitor_cmd('prop_get', path=path, property=prop)

    def stop(self) -> None:
        proc_handle = self._handles.procs.get('simavr')
        if self.running():
            try:
                self._monitor_cmd('quit')
            except (Error, OSError):
                pass
            if proc_handle is not None:
                try:
                    psutil.Process(proc_handle.pid).wait(timeout=1.5)
                except (psutil.Error, OSError):
                    pass

        self._close_monitor()
        super().stop()

        # Upstream pw_emu.core._stop_processes() formats `{proc}.pid` instead of
        # `{name}.pid`, leaving `<name>.pid` files behind. Clean them explicitly.
        for name in self._handles.procs:
            try:
                (self._wdir / f'{name}.pid').unlink()
            except FileNotFoundError:
                pass
        for name, chan in self._handles.channels.items():
            if chan.type == 'pty':
                link_path = self._wdir / name
                if link_path.is_symlink():
                    try:
                        link_path.unlink()
                    except OSError:
                        pass
        for artifact in ('channels.json', 'uart0.pty'):
            try:
                (self._wdir / artifact).unlink()
            except FileNotFoundError:
                pass
