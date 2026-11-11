#!/usr/bin/env bash
# Emit a reproducible source audit for the exact Pigweed gitlink checked out by CI.
set -euo pipefail
PW_ROOT="${GITHUB_WORKSPACE:-$(pwd)}/third_party/pigweed"
OUT="${GITHUB_WORKSPACE:-$(pwd)}/build/pw-upstream-audit.txt"
mkdir -p "$(dirname "$OUT")"
{
  echo "=== Pigweed revision ==="
  git -C "$PW_ROOT" rev-parse HEAD
  echo
  echo "=== Relevant CMake target declarations ==="
  grep -R -n -E 'pw_add_library|pw_target_link_targets' \\
    "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" 2>/dev/null || true
  echo
  echo "=== Relevant CMakeLists files ==="
  find "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" "$PW_ROOT/pw_sync" \
    -name CMakeLists.txt -type f -print 2>/dev/null | sort
  for file in \
    pw_rpc/CMakeLists.txt \
    pw_rpc/raw/CMakeLists.txt \
    pw_rpc/public/pw_rpc/channel.h \
    pw_rpc/public/pw_rpc/channel_output.h \
    pw_rpc/public/pw_rpc/server.h \
    pw_rpc/public/pw_rpc/raw/server_reader_writer.h \
    pw_rpc/public/pw_rpc/internal/config.h \
    pw_rpc/public/pw_rpc/internal/lock.h \
    pw_hdlc/CMakeLists.txt \
    pw_hdlc/public/pw_hdlc/rpc_channel.h \
    pw_hdlc/public/pw_hdlc/encoder.h \
    pw_thread/public/pw_thread/thread_core.h \
    pw_transfer/CMakeLists.txt \
    pw_transfer/public/pw_transfer/transfer.h \
    pw_transfer/public/pw_transfer/transfer_thread.h \
    pw_transfer/transfer_thread.cc \
    pw_transfer/client.cc \
    pw_sync/CMakeLists.txt \
    pw_sync/backend.cmake
  do
    path="$PW_ROOT/$file"
    echo
    echo "===== $file ====="
    if [[ -f "$path" ]]; then sed -n '1,320p' "$path"; else echo "NOT PRESENT AT PINNED REVISION"; fi
  done
} | tee "$OUT"
echo "Pigweed source audit written to: $OUT"
