"""Pure ELF symbolization and stack-watermark helpers for simavr diagnostics."""

from __future__ import annotations

import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

# avr-nm emits SRAM/data-space symbols with the 0x800000 linker offset.
_AVR_DATA_SPACE_MASK = 0xFFFF
_DEFAULT_MAX_PEAK_FRAMES = 16


def _run_avr_tool(args: Sequence[str], *, timeout: float = 10.0) -> str | None:
    try:
        result = subprocess.run(
            list(args),
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout if result.returncode == 0 else None


def avr_symbol_address(
    firmware_path: Path,
    symbol: str,
    *,
    data_space: bool = True,
) -> int | None:
    """Resolve an ELF symbol address via avr-nm, returning None if unavailable."""
    output = _run_avr_tool(['avr-nm', str(firmware_path)])
    if output is None:
        return None
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == symbol:
            address = int(fields[0], 16)
            return (address & _AVR_DATA_SPACE_MASK) if data_space else address
    return None


def symbolize_addresses(firmware_path: Path, byte_addresses: Sequence[int]) -> list[str]:
    """Symbolize AVR flash byte addresses via avr-addr2line."""
    if not byte_addresses:
        return []
    output = _run_avr_tool(
        ['avr-addr2line', '-f', '-C', '-p', '-e', str(firmware_path), *(hex(a) for a in byte_addresses)]
    )
    return output.splitlines() if output else []


def scan_return_addresses(
    stack: bytes,
    address_size: int,
    text_end: int,
    *,
    max_frames: int = _DEFAULT_MAX_PEAK_FRAMES,
) -> list[int]:
    """Extract candidate flash byte return addresses from an AVR stack snapshot.

    AVR ``CALL`` pushes the word address low-byte first onto a downward-growing
    stack, so reading upward from ``SP + 1`` yields big-endian word addresses.
    """
    if address_size <= 0 or text_end <= 0 or len(stack) < address_size:
        return []
    candidates: list[int] = []
    for start in range(len(stack) - address_size + 1):
        word_address = int.from_bytes(stack[start : start + address_size], 'big')
        byte_address = word_address * 2
        if 0 < byte_address < text_end and byte_address not in candidates:
            candidates.append(byte_address)
            if len(candidates) >= max_frames:
                break
    return candidates


def symbolize_diagnostics(
    firmware_path: Path,
    diagnostics: Mapping[str, Any],
    *,
    max_frames: int = _DEFAULT_MAX_PEAK_FRAMES,
) -> dict[str, Any]:
    """Enrich simavr ``diagnostics.json`` / ``stack_stats`` with ELF symbols and stack metrics."""
    ramend_raw = diagnostics.get('ramend')
    ramend = int(ramend_raw) if isinstance(ramend_raw, int) else None
    min_sp_raw = diagnostics.get('min_sp')
    min_sp_valid = bool(diagnostics.get('min_sp_valid', min_sp_raw is not None))
    min_sp = (
        int(min_sp_raw)
        if min_sp_valid and isinstance(min_sp_raw, int) and ramend is not None and 0 <= min_sp_raw <= ramend
        else None
    )

    heap_start = avr_symbol_address(firmware_path, '__heap_start', data_space=True)
    text_end = avr_symbol_address(firmware_path, '_etext', data_space=False)

    stack_used = (ramend - min_sp) if (ramend is not None and min_sp is not None) else None
    stack_budget = (ramend - heap_start) if (ramend is not None and heap_start is not None) else None
    stack_margin = (min_sp - heap_start) if (min_sp is not None and heap_start is not None) else None
    stack_overflow = (min_sp < heap_start) if (min_sp is not None and heap_start is not None) else None

    address_size_raw = diagnostics.get('address_size', 2)
    address_size = int(address_size_raw) if isinstance(address_size_raw, int) else 2
    peak_pc_raw = diagnostics.get('peak_pc')
    peak_pc = int(peak_pc_raw) if isinstance(peak_pc_raw, int) else None
    peak_stack_hex = str(diagnostics.get('peak_stack_hex', '')).replace(' ', '')
    try:
        peak_stack = bytes.fromhex(peak_stack_hex) if peak_stack_hex else b''
    except ValueError:
        peak_stack = b''

    peak_function = None
    if peak_pc is not None and min_sp is not None:
        sym = symbolize_addresses(firmware_path, [peak_pc])
        peak_function = sym[0] if sym else None

    frames = scan_return_addresses(peak_stack, address_size, text_end, max_frames=max_frames) if text_end else []
    call_chain_candidates = symbolize_addresses(firmware_path, frames)

    crashed = bool(diagnostics.get('crashed', False))
    crash_pc_raw = diagnostics.get('crash_pc')
    crash_pc = int(crash_pc_raw) if crashed and isinstance(crash_pc_raw, int) else None
    crash_function = None
    if crash_pc is not None:
        sym = symbolize_addresses(firmware_path, [crash_pc])
        crash_function = sym[0] if sym else None

    return {
        'schema_version': 1,
        'target': firmware_path.parent.name,
        'min_sp_valid': min_sp is not None,
        'min_sp': min_sp,
        'ramend': ramend,
        'heap_start': heap_start,
        'stack_used_bytes': stack_used,
        'stack_budget_bytes': stack_budget,
        'stack_margin_bytes': stack_margin,
        'stack_overflowed_into_bss': stack_overflow,
        'peak_pc': peak_pc if min_sp is not None else None,
        'peak_function': peak_function,
        'call_chain_candidates': call_chain_candidates,
        'crashed': crashed,
        'crash_pc': crash_pc,
        'crash_sp': int(diagnostics['crash_sp']) if crashed and isinstance(diagnostics.get('crash_sp'), int) else None,
        'crash_sreg': (
            int(diagnostics['crash_sreg']) if crashed and isinstance(diagnostics.get('crash_sreg'), int) else None
        ),
        'crash_cycle': (
            int(diagnostics['crash_cycle']) if crashed and isinstance(diagnostics.get('crash_cycle'), int) else None
        ),
        'crash_function': crash_function,
    }
