#!/usr/bin/env python3
"""Apply patches/0001-avr-pw-transfer-and-protobuf.patch to third_party/pigweed idempotently."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PW_ROOT = ROOT / "third_party" / "pigweed"
PATCH_FILE = ROOT / "patches" / "0001-avr-pw-transfer-and-protobuf.patch"
EXPECTED_PIGWEED_SHA = "55873baa1b37eaf7d0a830515a0846e3fc3a79e2"


def main() -> int:
    if not PW_ROOT.is_dir():
        print(f"ERROR: Pigweed directory not found: {PW_ROOT}", file=sys.stderr)
        return 1

    actual_sha = subprocess.check_output(
        ["git", "-C", str(PW_ROOT), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual_sha != EXPECTED_PIGWEED_SHA:
        print(
            f"ERROR: Unexpected Pigweed commit {actual_sha}; expected {EXPECTED_PIGWEED_SHA}",
            file=sys.stderr,
        )
        return 1

    if not PATCH_FILE.is_file():
        print(f"ERROR: Patch file not found: {PATCH_FILE}", file=sys.stderr)
        return 1

    # Check if already applied (reverse check passes)
    rev_check = subprocess.run(
        ["git", "-C", str(PW_ROOT), "apply", "--reverse", "--check", str(PATCH_FILE)],
        capture_output=True,
    )
    if rev_check.returncode == 0:
        print("[apply_pigweed_patches] Pigweed AVR patch already applied.")
        return 0

    # Verify that the patch applies cleanly
    fwd_check = subprocess.run(
        ["git", "-C", str(PW_ROOT), "apply", "--check", str(PATCH_FILE)],
        capture_output=True,
        text=True,
    )
    if fwd_check.returncode != 0:
        print(
            f"ERROR: Pigweed AVR patch cannot be applied cleanly:\n{fwd_check.stderr}",
            file=sys.stderr,
        )
        return 1

    # Apply the patch using git apply
    subprocess.run(
        ["git", "-C", str(PW_ROOT), "apply", str(PATCH_FILE)],
        check=True,
        capture_output=True,
    )
    print(f"[apply_pigweed_patches] Applied {PATCH_FILE.name} successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
