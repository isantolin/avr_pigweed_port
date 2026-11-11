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
  echo "=== Protobuf compiler version references ==="
  grep -R -n -E "protoc.*version|PROTOC_VERSION|protobuf.*version|experimental_editions" "$PW_ROOT/pw_protobuf_compiler" "$PW_ROOT/pw_env_setup" 2>/dev/null | head -n 120 || true
  echo
  echo "=== Proto Editions declarations in required modules ==="
  grep -R -n -E '^[[:space:]]*edition[[:space:]]*=' "$PW_ROOT/pw_protobuf" "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" --include='*.proto' 2>/dev/null || true
  echo
  echo "=== Protobuf AVR configuration references ==="
  grep -R -n -E 'double|DOUBLE|FLOAT|PW_PROTOBUF_' "$PW_ROOT/pw_protobuf/public/pw_protobuf" 2>/dev/null | head -n 160 || true
  echo
  echo "=== AVR protobuf static assertions and integer-width assumptions ==="
  grep -R -n -E 'four words|1 << (16|29)|sizeof\(double\)|sizeof\(uintptr_t\)|sizeof\(void\*)' "$PW_ROOT/pw_protobuf/public/pw_protobuf" 2>/dev/null | head -n 200 || true
  echo
  echo "=== RPC Python plugin package layout ==="
  find "$PW_ROOT/pw_rpc/py" "$PW_ROOT/pw_rpc2/py" -maxdepth 4 -type f -print 2>/dev/null | sort || true
  echo
  echo "=== Transfer thread/synchronization dependencies ==="
  grep -R -n -E 'sync::Mutex|BinarySemaphore|TimedThreadNotification|ThreadCore|event_notification_' "$PW_ROOT/pw_transfer" 2>/dev/null | head -n 200 || true
  echo
  echo "=== Double fields in RPC and transfer protocol schemas ==="
  grep -R -n -E '(^|[[:space:]])double[[:space:]]' "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" --include='*.proto' 2>/dev/null || true
  echo
  echo "=== Relevant CMake target declarations ==="
  grep -R -n -E 'pw_add_library|pw_target_link_targets' "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" 2>/dev/null || true
  echo
  echo "=== Relevant CMakeLists files ==="
  find "$PW_ROOT/pw_rpc" "$PW_ROOT/pw_transfer" "$PW_ROOT/pw_sync" -name CMakeLists.txt -type f -print 2>/dev/null | sort
  for file in \
    pw_rpc/CMakeLists.txt \
    pw_rpc/raw/CMakeLists.txt \
    pw_rpc/public/pw_rpc/channel.h \
    pw_rpc/public/pw_rpc/server.h \
    pw_rpc/public/pw_rpc/raw/server_reader_writer.h \
    pw_rpc/public/pw_rpc/internal/config.h \
    pw_rpc/public/pw_rpc/internal/lock.h \
    pw_rpc/py/pw_rpc/plugin.py \
    pw_rpc/py/pw_rpc/codegen.py \
    pw_rpc/py/pw_rpc/descriptors.py \
    pw_rpc/py/pw_rpc/packets.py \
    pw_rpc/py/pw_rpc/plugin_raw.py \
    pw_rpc/py/pw_rpc/codegen_raw.py \
    pw_rpc2/py/pw_rpc2/plugin_raw.py \
    pw_rpc2/py/pw_rpc2/codegen_raw.py \
    pw_hdlc/CMakeLists.txt \
    pw_hdlc/public/pw_hdlc/rpc_channel.h \
    pw_hdlc/public/pw_hdlc/encoder.h \
    pw_hdlc/public/pw_hdlc/decoder.h \
    pw_thread/public/pw_thread/thread_core.h \
    pw_protobuf_compiler/proto.cmake \
    pw_protobuf_compiler/py/pw_protobuf_compiler/generate_protos.py \
    pw_protobuf/CMakeLists.txt \
    pw_protobuf/public/pw_protobuf/internal/config.h \
    pw_protobuf/public/pw_protobuf/decoder.h \
    pw_protobuf/public/pw_protobuf/encoder.h \
    pw_protobuf/public/pw_protobuf/wire_format.h \
    pw_protobuf/public/pw_protobuf/internal/codegen.h \
    pw_protobuf/public/pw_protobuf/stream_decoder.h \
    pw_protobuf/public/pw_protobuf/buffer_encoder.h \
    pw_protobuf/public/pw_protobuf/message.h \
    pw_protobuf/public/pw_protobuf/config.h \
    pw_protobuf/public/pw_protobuf/serialized_size.h \
    pw_protobuf/py/pw_protobuf/plugin.py \
    pw_protobuf/py/pw_protobuf/options.py \
    pw_protobuf/py/pw_protobuf/__init__.py \
    pw_status/public/pw_status/status_with_size.h \
    pw_protobuf/decoder.cc \
    pw_protobuf/encoder.cc \
    pw_protobuf/message.cc \
    pw_protobuf/find.cc \
    pw_protobuf/map_utils.cc \
    pw_transfer/public/pw_transfer/internal/context.h \
    pw_transfer/public/pw_transfer/internal/client_context.h \
    pw_transfer/public/pw_transfer/internal/server_context.h \
    pw_transfer/public/pw_transfer/internal/event.h \
    pw_sync/public/pw_sync/no_lock.h \
    pw_transfer/public/pw_transfer/internal/chunk.h \
    pw_transfer/chunk.cc \
    pw_protobuf/CMakeLists.txt \
    pw_protobuf/public/pw_protobuf/internal/config.h \
    pw_protobuf/public/pw_protobuf/decoder.h \
    pw_protobuf/public/pw_protobuf/encoder.h \
    pw_transfer/public/pw_transfer/internal/chunk.h \
    pw_transfer/chunk.cc \
    pw_transfer/CMakeLists.txt \
    pw_transfer/public/pw_transfer/transfer.h \
    pw_transfer/public/pw_transfer/client.h \
    pw_transfer/public/pw_transfer/handler.h \
    pw_transfer/public/pw_transfer/transfer_thread.h \
    pw_transfer/transfer_thread.cc \
    pw_transfer/client.cc \
    pw_sync/CMakeLists.txt \
    pw_sync/backend.cmake
  do
    path="$PW_ROOT/$file"
    echo
    echo "===== $file ====="
    if [[ -f "$path" ]]; then sed -n '1,1200p' "$path"; else echo "NOT PRESENT AT PINNED REVISION"; fi
  done
} | tee "$OUT"
echo "Pigweed source audit written to: $OUT"
