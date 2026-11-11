#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PW_ROOT="$ROOT/third_party/pigweed"
PATCH="$ROOT/patches/pigweed/avr-protobuf-compat.patch"
EXPECTED="55873baa1b37eaf7d0a830515a0846e3fc3a79e2"
if [[ ! -d "$PW_ROOT/.git" && ! -f "$PW_ROOT/.git" ]]; then
  echo "Pigweed submodule is not initialized: $PW_ROOT" >&2
  exit 1
fi
ACTUAL="$(git -C "$PW_ROOT" rev-parse HEAD)"
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
  echo "Unexpected Pigweed revision: $ACTUAL (expected $EXPECTED)" >&2
  exit 1
fi
if git -C "$PW_ROOT" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "AVR protobuf compatibility patch already applied"
  exit 0
fi
git -C "$PW_ROOT" apply --check "$PATCH"
git -C "$PW_ROOT" apply "$PATCH"
echo "Applied AVR protobuf compatibility patch to pinned Pigweed $EXPECTED"
