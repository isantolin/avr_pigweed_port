#!/usr/bin/env bash
# File: bootstrap_avr_pigweed.sh
set -euo pipefail

PROJECT_DIR="avr_pigweed_port"
mkdir -p "${PROJECT_DIR}/pw_assert_avr/public/pw_assert_backend"
mkdir -p "${PROJECT_DIR}/pw_assert_avr/public_overrides/pw_assert_backend"
mkdir -p "${PROJECT_DIR}/src"
cd "${PROJECT_DIR}"

if [ ! -d ".git" ]; then
  git init
fi

if [ ! -d "third_party/pigweed" ]; then
  git submodule add --depth 1 https://pigweed.googlesource.com/pigweed/pigweed third_party/pigweed
fi

if [ ! -d "third_party/avr-libstdcpp" ]; then
  git submodule add --depth 1 https://github.com/modm-io/avr-libstdcpp.git third_party/avr-libstdcpp
fi

echo "Directory structure and submodules ready in $(pwd)."
echo "Copy CMakeLists.txt, avr-toolchain.cmake, pw_assert_avr/*, and src/* files, then run:"
echo "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=avr-toolchain.cmake -DAVR_CRC32_IMPL=PW_CHECKSUM_CRC32_4BITS"
echo "  cmake --build build"
echo "  cmake --build build --target run_tests"
