#!/usr/bin/env bash
# Initialize the submodules declared by this repository.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Run this script from a cloned avr_pigweed_port repository." >&2
  exit 1
fi

git submodule update --init --recursive --depth 1

echo "Pigweed and AVR standard-library submodules are initialized in ${REPO_ROOT}."
echo "Configure with:"
echo "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake -DAVR_MCU=atmega2560 -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS"
echo "  cmake --build build --target all run_tests"
