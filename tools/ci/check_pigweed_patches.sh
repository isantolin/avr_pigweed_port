#!/usr/bin/env bash
# Verify that patches/0001-avr-pw-transfer-and-protobuf.patch applies cleanly
# and that apply_pigweed_patches.py is strictly idempotent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PW_ROOT="${ROOT}/third_party/pigweed"
PATCH_FILE="${ROOT}/patches/0001-avr-pw-transfer-and-protobuf.patch"
APPLY="${ROOT}/tools/ci/apply_pigweed_patches.py"

# Reset pigweed to clean upstream state
git -C "${PW_ROOT}" checkout -- .
git -C "${PW_ROOT}" clean -fdx

# First application
python3 "${APPLY}"
first="$(git -C "${PW_ROOT}" diff --full-index)"

# Second application (must be a no-op)
python3 "${APPLY}"
second="$(git -C "${PW_ROOT}" diff --full-index)"

if [[ "${first}" != "${second}" ]]; then
    echo "ERROR: apply_pigweed_patches.py is not idempotent" >&2
    exit 1
fi

echo "[OK] apply_pigweed_patches.py is idempotent and matches patches/0001-avr-pw-transfer-and-protobuf.patch"
