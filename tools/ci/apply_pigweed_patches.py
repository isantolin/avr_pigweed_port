#!/usr/bin/env python3
"""Apply the small, pinned AVR portability delta required by Pigweed PWPB."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
PW_ROOT = ROOT / "third_party" / "pigweed"
EXPECTED_PIGWEED_SHA = "55873baa1b37eaf7d0a830515a0846e3fc3a79e2"


def replace_once(relative_path: str, old: str, new: str) -> None:
    path = PW_ROOT / relative_path
    content = path.read_text()
    if new in content:
        print(f"already patched: {relative_path}")
        return
    count = content.count(old)
    if count != 1:
        raise RuntimeError(
            f"{relative_path}: expected one source match, found {count}"
        )
    path.write_text(content.replace(old, new, 1))
    print(f"patched: {relative_path}")


def replace_all(relative_path: str, old: str, new: str) -> None:
    path = PW_ROOT / relative_path
    content = path.read_text()
    if old not in content:
        if new in content:
            print(f"already patched: {relative_path}")
            return
        raise RuntimeError(f"{relative_path}: source pattern not found")
    path.write_text(content.replace(old, new))
    print(f"patched all occurrences: {relative_path}")


def main() -> int:
    actual = subprocess.check_output(
        ["git", "-C", str(PW_ROOT), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != EXPECTED_PIGWEED_SHA:
        raise RuntimeError(
            f"unexpected Pigweed SHA {actual}; expected {EXPECTED_PIGWEED_SHA}"
        )

    replace_once(
        "pw_rpc/py/pw_rpc/__init__.py",
        "from pw_rpc.client import Client",
        """from pkgutil import extend_path

# Generated pw_rpc.protos Python modules live in a sibling build directory.
# Extend this package so pw_rpc.internal.packet_pb2 is importable by protoc.
__path__ = extend_path(__path__, __name__)

from pw_rpc.client import Client""",
    )
    replace_once(
        "pw_transfer/public/pw_transfer/internal/context.h",
        """  // The session ID as unsigned instead of uint32_t so it can be used with %u.
  unsigned id_for_log() const {
    static_assert(sizeof(unsigned) >= sizeof(session_id_));
    return static_cast<unsigned>(session_id_);
  }""",
        """  // Preserve the full session ID on targets with 16-bit unsigned.
  unsigned long id_for_log() const {
    static_assert(sizeof(unsigned long) >= sizeof(session_id_));
    return static_cast<unsigned long>(session_id_);
  }""",
    )

    # AVR's unsigned is 16-bit while transfer session IDs are 32-bit. Keep the
    # full value and use the matching %lu format in every log call.
    context_cc = PW_ROOT / "pw_transfer" / "context.cc"
    context_content = context_cc.read_text()
    cursor = 0
    patched_log_formats = 0
    while True:
        id_position = context_content.find("id_for_log()", cursor)
        if id_position < 0:
            break
        macro_start = context_content.rfind("PW_LOG_", 0, id_position)
        macro_end = context_content.find(");", id_position)
        if macro_start < 0 or macro_end < 0:
            raise RuntimeError("could not locate PW_LOG call for id_for_log()")
        macro_end += 2
        call = context_content[macro_start:macro_end]
        id_offset = call.find("id_for_log()")
        # The CMake configure step may run more than once in the same checkout
        # (for example, the 328P and Mega2560 probe builds). If the ID's first
        # format slot is already %lu, do not rewrite the next %u argument.
        format_prefix = call[:id_offset]
        if "%lu" in format_prefix:
            pass
        else:
            format_offset = call.find("%u")
            if format_offset >= 0 and format_offset < id_offset:
                call = call[:format_offset] + "%lu" + call[format_offset + 2:]
                context_content = (
                    context_content[:macro_start] + call + context_content[macro_end:]
                )
                patched_log_formats += 1
            else:
                raise RuntimeError("id_for_log() log call has no matching %u format")
        cursor = macro_start + len(call)
    if patched_log_formats:
        context_cc.write_text(context_content)
        print(f"patched {patched_log_formats} transfer log format specifiers")
    elif "id_for_log()" in context_content and "%lu" not in context_content:
        raise RuntimeError("transfer log format patch did not match any call")
    replace_once(
        "pw_protobuf/public/pw_protobuf/wire_format.h",
        "inline constexpr static uint32_t kMaxFieldNumber = (1u << 29) - 1;",
        "inline constexpr static uint32_t kMaxFieldNumber = (uint32_t{1} << 29) - 1;",
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/internal/codegen.h",
        "static constexpr unsigned int kMaxFieldSize = (1u << 16) - 1;",
        "static constexpr uint32_t kMaxFieldSize = (uint32_t{1} << 16) - 1;",
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/internal/codegen.h",
        '''static_assert(sizeof(MessageField) <= sizeof(size_t) * 4,
              "MessageField should be four words or less");''',
        '''static_assert(
    sizeof(MessageField) <=
        (sizeof(size_t) < sizeof(uint32_t) ? sizeof(uint32_t) * 4
                                           : sizeof(size_t) * 4),
    "MessageField should be four 32-bit words or less on 16-bit targets");''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/decoder.h",
        '''  Status ReadDouble(double* out) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t must be the same size for protobufs");
    return ReadFixed(out);
  }''',
        '''  Status ReadDouble(double* out) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)out;
      return Status::Unimplemented();
    } else {
      return ReadFixed(out);
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/encoder.h",
        '''  Status WriteDouble(uint32_t field_number, double value) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t are not the same size");
    uint64_t integral_value;
    std::memcpy(&integral_value, &value, sizeof(value));
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(endian::little, integral_value);
    return WriteFixed(field_number, data);
  }''',
        '''  Status WriteDouble(uint32_t field_number, double value) {
#if defined(__SIZEOF_DOUBLE__) && __SIZEOF_DOUBLE__ == 8
    uint64_t integral_value;
    std::memcpy(&integral_value, &value, sizeof(value));
    std::array<std::byte, sizeof(value)> data =
        bytes::CopyInOrder(endian::little, integral_value);
    return WriteFixed(field_number, data);
#else
    (void)field_number;
    (void)value;
    return Status::Unimplemented();
#endif
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/encoder.h",
        '''  Status WritePackedDouble(uint32_t field_number, span<const double> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(double));
  }''',
        '''  Status WritePackedDouble(uint32_t field_number, span<const double> values) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)values;
      return Status::Unimplemented();
    } else {
      return WritePackedFixed(field_number, as_bytes(values), sizeof(double));
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/encoder.h",
        '''  Status WriteRepeatedDouble(uint32_t field_number,
                             const pw::Vector<double>& values) {
    return WritePackedFixed(field_number,
                            as_bytes(span(values.data(), values.size())),
                            sizeof(double));
  }''',
        '''  Status WriteRepeatedDouble(uint32_t field_number,
                             const pw::Vector<double>& values) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)values;
      return Status::Unimplemented();
    } else {
      return WritePackedFixed(field_number,
                              as_bytes(span(values.data(), values.size())),
                              sizeof(double));
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/stream_decoder.h",
        '''  Result<double> ReadDouble() {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t must be the same size for protobufs");
    return ReadFixedField<double>();
  }''',
        '''  Result<double> ReadDouble() {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      return Status::Unimplemented();
    } else {
      return ReadFixedField<double>();
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/stream_decoder.h",
        '''  StatusWithSize ReadPackedDouble(span<double> out) {
    static_assert(sizeof(double) == sizeof(uint64_t),
                  "Double and uint64_t must be the same size for protobufs");
    return ReadPackedFixedField(as_writable_bytes(out), sizeof(double));
  }''',
        '''  StatusWithSize ReadPackedDouble(span<double> out) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)out;
      return StatusWithSize::Unimplemented();
    } else {
      return ReadPackedFixedField(as_writable_bytes(out), sizeof(double));
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/stream_decoder.h",
        '''  Status ReadRepeatedDouble(pw::Vector<double>& out) {
    return ReadRepeatedFixedField<double>(out);
  }''',
        '''  Status ReadRepeatedDouble(pw::Vector<double>& out) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)out;
      return Status::Unimplemented();
    } else {
      return ReadRepeatedFixedField<double>(out);
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/buffer_encoder.h",
        '#include "pw_bytes/span.h"',
        '#include "pw_assert/check.h"\n#include "pw_bytes/span.h"',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/buffer_encoder.h",
        '''  void UncheckedWriteDouble(uint32_t field_number, double value) {
    UncheckedWriteTag(field_number, WireType::kFixed64);
    UncheckedWriteBytes(&value, sizeof(value));
  }
  pw::Status WriteDouble(uint32_t field_number, double value) {
    if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesDouble)) {
      UncheckedWriteDouble(field_number, value);
    }
    return status();
  }''',
        '''  void UncheckedWriteDouble(uint32_t field_number, double value) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)value;
      PW_CRASH("Protobuf double requires a 64-bit native double");
    } else {
      UncheckedWriteTag(field_number, WireType::kFixed64);
      UncheckedWriteBytes(&value, sizeof(value));
    }
  }
  pw::Status WriteDouble(uint32_t field_number, double value) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)value;
      return Status::Unimplemented();
    } else {
      if (EnsureSpace(TagSizeBytes(field_number) + kMaxSizeBytesDouble)) {
        UncheckedWriteDouble(field_number, value);
      }
      return status();
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/buffer_encoder.h",
        '''  void UncheckedWritePackedDouble(uint32_t field_number,
                                  span<const double> values) {
    UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(double));
  }
  pw::Status WritePackedDouble(uint32_t field_number,
                               span<const double> values) {
    return WritePackedFixed(field_number, as_bytes(values), sizeof(double));
  }''',
        '''  void UncheckedWritePackedDouble(uint32_t field_number,
                                  span<const double> values) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)values;
      PW_CRASH("Protobuf double requires a 64-bit native double");
    } else {
      UncheckedWritePackedFixed(field_number, as_bytes(values), sizeof(double));
    }
  }
  pw::Status WritePackedDouble(uint32_t field_number,
                               span<const double> values) {
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)values;
      return Status::Unimplemented();
    } else {
      return WritePackedFixed(field_number, as_bytes(values), sizeof(double));
    }
  }''',
    )
    replace_once(
        "pw_protobuf/public/pw_protobuf/serialized_size.h",
        "return TagSizeBytes(field_number) + sizeof(double);",
        "return TagSizeBytes(field_number) + kMaxSizeBytesDouble;",
    )


    # Cooperative pw_transfer: the same upstream state machine is polled by
    # the AVR owner loop instead of inheriting a blocking ThreadCore.
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        '''#include "pw_span/span.h"
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_thread/thread_core.h"
#include "pw_transfer/handler.h"''',
        '''#include <array>
#include <cstddef>
#include <cstring>

#ifndef PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY
#define PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY 2
#endif
#ifndef PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE
#define PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE 128
#endif

#include "pw_span/span.h"
#include "pw_assert/check.h"
#if !defined(PW_TRANSFER_COOPERATIVE)
#include "pw_sync/binary_semaphore.h"
#include "pw_sync/timed_thread_notification.h"
#include "pw_thread/thread_core.h"
#endif
#include "pw_transfer/handler.h"''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        "class TransferThread : public thread::ThreadCore {",
        """class TransferThread
#if !defined(PW_TRANSFER_COOPERATIVE)
    : public thread::ThreadCore
#endif
{""",
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        '''  // For testing only: terminates the transfer thread with a kTerminate event.
  void Terminate();''',
        '''#if defined(PW_TRANSFER_COOPERATIVE)
  // Process at most budget queued events and check transfer deadlines.
  // Unavailable means more events remain; ResourceExhausted means the bounded
  // queue overflowed and active transfers were aborted rather than corrupted.
  Status Poll(size_t budget = 1);
#if defined(PW_TRANSFER_COOPERATIVE_TEST)
  // Test-only event injector for deterministic bounded-queue overflow tests.
  void EnqueueTestEvent() {
    if (!TryWaitForEventToProcess()) {
      event_overflowed_ = true;
      return;
    }
    next_event_ = {};
    next_event_.type = EventType::kClientEndTransfer;
    next_event_.end_transfer.id_type = IdentifierType::Session;
    next_event_.end_transfer.id = 0;
    next_event_.end_transfer.status = Status::Cancelled().code();
    next_event_.end_transfer.send_status_chunk = false;
    NotifyEvent();
  }

  size_t PendingEventsForTest() const { return event_queue_size_; }

  size_t ActiveClientTransfersForTest() const {
    size_t count = 0;
    for (const ClientContext& context : client_transfers_) {
      if (context.active()) ++count;
    }
    return count;
  }

  size_t ActiveServerTransfersForTest() const {
    size_t count = 0;
    for (const ServerContext& context : server_transfers_) {
      if (context.active()) ++count;
    }
    return count;
  }
#endif
#endif

  // For testing only: terminates the transfer thread with a kTerminate event.
  void Terminate();''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        '''  void WaitUntilEventIsProcessed() {
    next_event_ownership_.acquire();
    next_event_ownership_.release();
  }''',
        '''  void WaitUntilEventIsProcessed() {
#if defined(PW_TRANSFER_COOPERATIVE)
    (void)Poll(PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY);
#else
    next_event_ownership_.acquire();
    next_event_ownership_.release();
#endif
  }''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        "  void Run() final;\n\n  void HandleTimeouts();",
        """#if !defined(PW_TRANSFER_COOPERATIVE)
  void Run() final;
#endif

  bool HandleTimeouts();""",
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        '''  bool TryWaitForEventToProcess() {
    if constexpr (cfg::kWaitForEventProcessingIndefinitely) {
      next_event_ownership_.acquire();
      return true;
    }
    return next_event_ownership_.try_acquire_for(cfg::kEventProcessingTimeout);
  }''',
        '''  void NotifyEvent() {
#if defined(PW_TRANSFER_COOPERATIVE)
    if (event_queue_size_ >= PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY) {
      event_overflowed_ = true;
      event_slot_available_ = true;
      return;
    }
    CooperativeEventSlot& slot = event_queue_[event_queue_tail_];
    slot.event = next_event_;
    const std::byte* chunk_data = nullptr;
    size_t chunk_size = 0;
    if (slot.event.type == EventType::kClientChunk ||
        slot.event.type == EventType::kServerChunk) {
      chunk_data = slot.event.chunk.data;
      chunk_size = slot.event.chunk.size;
    } else if (slot.event.type == EventType::kNewClientTransfer ||
               slot.event.type == EventType::kNewServerTransfer) {
      chunk_data = slot.event.new_transfer.raw_chunk_data;
      chunk_size = slot.event.new_transfer.raw_chunk_size;
    }
    if (chunk_size > slot.chunk_storage.size()) {
      if (slot.event.type == EventType::kNewServerTransfer) {
        SendStatusChunk({
            .session_id = slot.event.new_transfer.session_id,
            .protocol_version = slot.event.new_transfer.protocol_version,
            .status = Status::ResourceExhausted().code(),
            .stream = slot.event.new_transfer.type == TransferType::kTransmit
                          ? TransferStream::kServerRead
                          : TransferStream::kServerWrite,
        });
      }
      event_overflowed_ = true;
      event_slot_available_ = true;
      return;
    }
    if (chunk_size != 0) {
      std::memcpy(slot.chunk_storage.data(), chunk_data, chunk_size);
      if (slot.event.type == EventType::kClientChunk ||
          slot.event.type == EventType::kServerChunk) {
        slot.event.chunk.data = slot.chunk_storage.data();
      } else {
        slot.event.new_transfer.raw_chunk_data = slot.chunk_storage.data();
      }
    }
    event_queue_tail_ =
        (event_queue_tail_ + 1) % PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY;
    ++event_queue_size_;
    event_slot_available_ = true;
#else
    event_notification_.release();
#endif
  }

  bool TryWaitForEventToProcess() {
#if defined(PW_TRANSFER_COOPERATIVE)
    if (!event_slot_available_ ||
        event_queue_size_ >= PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY) {
      event_overflowed_ = true;
      return false;
    }
    event_slot_available_ = false;
    return true;
#else
    if constexpr (cfg::kWaitForEventProcessingIndefinitely) {
      next_event_ownership_.acquire();
      return true;
    }
    return next_event_ownership_.try_acquire_for(cfg::kEventProcessingTimeout);
#endif
  }''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer_thread.h",
        '''  sync::TimedThreadNotification event_notification_;
  sync::BinarySemaphore next_event_ownership_;

  Event next_event_;''',
        '''#if defined(PW_TRANSFER_COOPERATIVE)
  struct CooperativeEventSlot {
    Event event{};
    std::array<std::byte, PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE>
        chunk_storage{};
  };
  static_assert(PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY >= 2,
                "The cooperative event queue must have at least two slots");
  std::array<CooperativeEventSlot, PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY>
      event_queue_{};
  size_t event_queue_head_ = 0;
  size_t event_queue_tail_ = 0;
  size_t event_queue_size_ = 0;
  bool event_slot_available_ = true;
  bool event_overflowed_ = false;
  bool resource_status_pending_ = false;
  enum class CooperativeRole : uint8_t { kIdle, kClient, kServer };
  CooperativeRole cooperative_role_ = CooperativeRole::kIdle;
  uint32_t cooperative_session_id_ = 0;
  bool cooperative_start_pending_ = false;
  void UpdateCooperativeRole();
#else
  sync::TimedThreadNotification event_notification_;
  sync::BinarySemaphore next_event_ownership_;
#endif

  Event next_event_;''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer.h",
        '#include "pw_sync/mutex.h"',
        '''#if defined(PW_TRANSFER_COOPERATIVE)
#include "pw_sync/no_lock.h"
#else
#include "pw_sync/mutex.h"
#endif''',
    )
    replace_once(
        "pw_transfer/public/pw_transfer/transfer.h",
        "  sync::Mutex resource_responder_mutex_;",
        '''#if defined(PW_TRANSFER_COOPERATIVE)
  sync::NoLock resource_responder_mutex_;
#else
  sync::Mutex resource_responder_mutex_;
#endif''',
    )

    replace_all(
        "pw_transfer/transfer_thread.cc",
        "event_notification_.release();",
        "NotifyEvent();",
    )
    replace_all(
        "pw_transfer/transfer_thread.cc",
        "next_event_ownership_.acquire();",
        "if (!TryWaitForEventToProcess()) { return; }",
    )

    transfer_cc = PW_ROOT / "pw_transfer" / "transfer_thread.cc"
    transfer_content = transfer_cc.read_text()
    run_start = transfer_content.index("void TransferThread::Run() {")
    if "#if !defined(PW_TRANSFER_COOPERATIVE)\nvoid TransferThread::Run()" not in transfer_content:
        run_end_marker = "\n}\n\nstd::optional<chrono::SystemClock::time_point>"
        run_end = transfer_content.index(run_end_marker, run_start) + 3
        run_block = transfer_content[run_start:run_end]
        transfer_content = (
            transfer_content[:run_start]
            + "#if !defined(PW_TRANSFER_COOPERATIVE)\n"
            + run_block
            + "#endif  // !PW_TRANSFER_COOPERATIVE\n"
            + transfer_content[run_end:]
        )

    stream_methods = (
        "void TransferThread::SetClientReadStream(",
        "void TransferThread::CloseClientReadStream(",
        "void TransferThread::SetClientWriteStream(",
        "void TransferThread::CloseClientWriteStream(",
        "void TransferThread::SetServerReadStream(",
        "void TransferThread::SetServerWriteStream(",
    )
    old_reserve = "  if (!TryWaitForEventToProcess()) { return; }"
    new_reserve = """#if !defined(PW_TRANSFER_COOPERATIVE)
  if (!TryWaitForEventToProcess()) { return; }
#endif"""
    for signature in stream_methods:
        method_start = transfer_content.index(signature)
        next_method = transfer_content.find(
            "\nvoid TransferThread::", method_start + len(signature)
        )
        method_end = len(transfer_content) if next_method < 0 else next_method
        method_block = transfer_content[method_start:method_end]
        if old_reserve in method_block:
            method_block = method_block.replace(old_reserve, new_reserve, 1)
        elif new_reserve not in method_block:
            raise RuntimeError(f"could not find stream reservation in {signature}")
        transfer_content = (
            transfer_content[:method_start] + method_block + transfer_content[method_end:]
        )

    stream_start = transfer_content.index("void TransferThread::SetClientReadStream")
    stream_tail = transfer_content[stream_start:]
    old_wait = "  NotifyEvent();\n  WaitUntilEventIsProcessed();"
    new_wait = """#if defined(PW_TRANSFER_COOPERATIVE)
  // In the cooperative build this callback runs on the same owner loop as
  // Poll(). Install the RPC stream synchronously so the first incoming
  // transfer chunk cannot arrive before its on_next callback is attached.
  HandleSetStreamEvent(next_event_.set_stream.stream,
                       next_event_.set_stream.behavior);
#else
  NotifyEvent();
  WaitUntilEventIsProcessed();
#endif"""
    wait_count = stream_tail.count(old_wait)
    if wait_count == 6:
        stream_tail = stream_tail.replace(old_wait, new_wait)
    elif stream_tail.count(new_wait) != 6:
        raise RuntimeError(
            f"expected six stream callback waits or six already-patched waits; found {wait_count}"
        )
    transfer_content = transfer_content[:stream_start] + stream_tail

    # Claim the single global transfer role only after StartTransfer has
    # staged its event and resolved the server handler.
    start_begin = transfer_content.index("void TransferThread::StartTransfer(")
    start_end = transfer_content.index("void TransferThread::ProcessChunk(", start_begin)
    start_block = transfer_content[start_begin:start_end]
    no_slot = """  if (!TryWaitForEventToProcess()) {
    return;
  }"""
    no_slot_replacement = """  if (!TryWaitForEventToProcess()) {
#if defined(PW_TRANSFER_COOPERATIVE)
    if (stream != nullptr) {
      if (on_completion) on_completion(Status::ResourceExhausted());
    } else {
      SendStatusChunk({
          .session_id = session_id,
          .protocol_version = version,
          .status = Status::ResourceExhausted().code(),
          .stream = type == TransferType::kTransmit
                        ? TransferStream::kServerRead
                        : TransferStream::kServerWrite,
      });
    }
#endif
    return;
  }"""
    if no_slot_replacement not in start_block:
        if start_block.count(no_slot) != 1:
            raise RuntimeError("could not find StartTransfer queue-full path")
        start_block = start_block.replace(no_slot, no_slot_replacement, 1)
    notify = "  NotifyEvent();"
    role_code = """#if defined(PW_TRANSFER_COOPERATIVE)
  if (next_event_.type == EventType::kNewClientTransfer ||
      next_event_.type == EventType::kNewServerTransfer) {
    const CooperativeRole requested_role =
        is_client_transfer ? CooperativeRole::kClient
                           : CooperativeRole::kServer;
    if (cooperative_role_ != CooperativeRole::kIdle &&
        (cooperative_role_ != requested_role ||
         cooperative_session_id_ != session_id ||
         cooperative_start_pending_)) {
      if (is_client_transfer) {
        if (staged_on_completion_) {
          staged_on_completion_(Status::Unavailable());
          staged_on_completion_ = nullptr;
        }
      } else {
        SendStatusChunk({
            .session_id = session_id,
            .protocol_version = version,
            .status = Status::Unavailable().code(),
            .stream = type == TransferType::kTransmit
                          ? TransferStream::kServerRead
                          : TransferStream::kServerWrite,
        });
      }
      event_slot_available_ = true;
      return;
    }
    cooperative_role_ = requested_role;
    cooperative_session_id_ = session_id;
    cooperative_start_pending_ = true;
  }
#endif
"""
    if role_code.strip() not in start_block:
        if start_block.count(notify) != 1:
            raise RuntimeError("could not find unique StartTransfer NotifyEvent")
        start_block = start_block.replace(notify, role_code + notify, 1)
    transfer_content = (
        transfer_content[:start_begin] + start_block + transfer_content[start_end:]
    )

    # A newly accepted transfer event consumes the pending role reservation.
    handle_marker = """  if (event.type == EventType::kNewClientTransfer) {
    // TODO(frolv): This is terrible."""
    handle_replacement = """  if (event.type == EventType::kNewClientTransfer ||
      event.type == EventType::kNewServerTransfer) {
#if defined(PW_TRANSFER_COOPERATIVE)
    cooperative_start_pending_ = false;
#endif
  }

  if (event.type == EventType::kNewClientTransfer) {
    // TODO(frolv): This is terrible."""
    if handle_replacement not in transfer_content:
        if transfer_content.count(handle_marker) != 1:
            raise RuntimeError("could not find unique new-transfer handling point")
        transfer_content = transfer_content.replace(handle_marker, handle_replacement, 1)

    # Release a pending role reservation if upstream could not allocate a context.
    null_context_marker = """  if (ctx == nullptr) {
    // No context was found. For new transfer events, report a
    // RESOURCE_EXHAUSTED error with starting the transfer."""
    null_context_replacement = """  if (ctx == nullptr) {
#if defined(PW_TRANSFER_COOPERATIVE)
    if (event.type == EventType::kNewClientTransfer ||
        event.type == EventType::kNewServerTransfer) {
      cooperative_start_pending_ = false;
      UpdateCooperativeRole();
    }
#endif
    // No context was found. For new transfer events, report a
    // RESOURCE_EXHAUSTED error with starting the transfer."""
    if null_context_replacement not in transfer_content:
        if transfer_content.count(null_context_marker) != 1:
            raise RuntimeError("could not find unique null-context branch")
        transfer_content = transfer_content.replace(
            null_context_marker, null_context_replacement, 1
        )

    # Update the global role after every state-machine event; it is released
    # only after both upstream context arrays are inactive.
    context_marker = "  ctx->HandleEvent(event);\n}"
    context_replacement = """  ctx->HandleEvent(event);
#if defined(PW_TRANSFER_COOPERATIVE)
  UpdateCooperativeRole();
#endif
}"""
    if context_replacement not in transfer_content:
        if transfer_content.count(context_marker) != 1:
            raise RuntimeError("could not find unique context event dispatch")
        transfer_content = transfer_content.replace(context_marker, context_replacement, 1)

    # The upstream resource-status callback is a single staged member. Keep
    # its original single-in-flight contract despite the bounded event queue.
    resource_start = transfer_content.index(
        "void TransferThread::EnqueueResourceEvent("
    )
    resource_brace = transfer_content.index("{", resource_start) + 1
    resource_guard = """
#if defined(PW_TRANSFER_COOPERATIVE)
  if (resource_status_pending_) {
    internal::ResourceStatus stats;
    callback(Status::Unavailable(), stats);
    return;
  }
#endif
"""
    if resource_guard.strip() not in transfer_content[
        resource_brace:resource_brace + 300
    ]:
        transfer_content = (
            transfer_content[:resource_brace]
            + resource_guard
            + transfer_content[resource_brace:]
        )

    resource_set = "  resource_status_callback_ = std::move(callback);"
    resource_set_replacement = resource_set + """
#if defined(PW_TRANSFER_COOPERATIVE)
  resource_status_pending_ = true;
#endif"""
    if resource_set_replacement not in transfer_content:
        if transfer_content.count(resource_set) != 1:
            raise RuntimeError("could not find unique resource-status callback staging")
        transfer_content = transfer_content.replace(
            resource_set, resource_set_replacement, 1
        )

    resource_state_start = transfer_content.index(
        "void TransferThread::GetResourceState("
    )
    resource_clear = "  resource_status_callback_ = nullptr;"
    clear_index = transfer_content.index(resource_clear, resource_state_start)
    clear_end = clear_index + len(resource_clear)
    clear_replacement = resource_clear + """
#if defined(PW_TRANSFER_COOPERATIVE)
  resource_status_pending_ = false;
#endif"""
    if "resource_status_pending_ = false;" not in transfer_content[
        clear_index:clear_index + 250
    ]:
        transfer_content = (
            transfer_content[:clear_index]
            + clear_replacement
            + transfer_content[clear_end:]
        )

    insert_marker = "std::optional<chrono::SystemClock::time_point>\nTransferThread::GetNextTransferTimeout() const {"
    if "Status TransferThread::Poll(size_t budget)" not in transfer_content:
        poll_code = '''#if defined(PW_TRANSFER_COOPERATIVE)
Status TransferThread::Poll(size_t budget) {
  if (event_overflowed_) {
    // Preserve accepted event ordering and consume staged objects before
    // aborting sessions after a dropped event. The initial count bounds work.
    const size_t accepted_before_overflow = event_queue_size_;
    for (size_t i = 0; i < accepted_before_overflow; ++i) {
      if (event_queue_size_ == 0) break;
      event_queue_head_ =
          (event_queue_head_ + 1) % PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY;
      --event_queue_size_;
    }
    event_queue_head_ = event_queue_tail_;
    if (resource_status_pending_ && resource_status_callback_) {
      internal::ResourceStatus stats;
      resource_status_callback_(Status::ResourceExhausted(), stats);
      resource_status_callback_ = nullptr;
    }
    Event abort_event{};
    abort_event.type = EventType::kClientEndTransfer;
    abort_event.end_transfer = {
        .id_type = IdentifierType::Session,
        .id = 0,
        .status = Status::ResourceExhausted().code(),
        .send_status_chunk = true,
    };
    for (ClientContext& context : client_transfers_) {
      if (context.active()) {
        abort_event.end_transfer.id = context.session_id();
        context.HandleEvent(abort_event);
      }
    }
    abort_event.type = EventType::kServerEndTransfer;
    for (ServerContext& context : server_transfers_) {
      if (context.active()) {
        abort_event.end_transfer.id = context.session_id();
        context.HandleEvent(abort_event);
      }
    }
    event_queue_head_ = event_queue_tail_;
    event_queue_size_ = 0;
    resource_status_pending_ = false;
    event_overflowed_ = false;
    if (cooperative_start_pending_ && staged_on_completion_) {
      staged_on_completion_(Status::ResourceExhausted());
      staged_on_completion_ = nullptr;
    }
    cooperative_start_pending_ = false;
    UpdateCooperativeRole();
    return Status::ResourceExhausted();
  }

  size_t processed = 0;
  while (event_queue_size_ != 0 && processed < budget) {
    CooperativeEventSlot& slot = event_queue_[event_queue_head_];
    HandleEvent(slot.event);
    event_queue_head_ =
        (event_queue_head_ + 1) % PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY;
    --event_queue_size_;
    ++processed;
  }
  (void)HandleTimeouts();
  UpdateCooperativeRole();
  return event_queue_size_ != 0 ? Status::Unavailable() : OkStatus();
}

void TransferThread::UpdateCooperativeRole() {
  if (cooperative_start_pending_) {
    return;
  }
  for (const Context& context : client_transfers_) {
    if (context.active()) return;
  }
  for (const Context& context : server_transfers_) {
    if (context.active()) return;
  }
  cooperative_role_ = CooperativeRole::kIdle;
  cooperative_session_id_ = 0;
}
#endif  // PW_TRANSFER_COOPERATIVE

bool TransferThread::HandleTimeouts() {
  bool did_work = false;
  for (Context& context : client_transfers_) {
    if (context.timed_out()) {
      context.HandleEvent({.type = EventType::kClientTimeout});
      did_work = true;
    }
  }
  for (Context& context : server_transfers_) {
    if (context.timed_out()) {
      context.HandleEvent({.type = EventType::kServerTimeout});
      did_work = true;
    }
  }
  return did_work;
}

'''
        if insert_marker not in transfer_content:
            raise RuntimeError("could not find timeout insertion point in transfer_thread.cc")
        transfer_content = transfer_content.replace(insert_marker, poll_code + insert_marker, 1)
    # ResourceStatus contains scalar fields without default member
    # initializers. Zero-initialize every temporary passed to callbacks.
    transfer_content = transfer_content.replace(
        "internal::ResourceStatus stats;",
        "internal::ResourceStatus stats{};",
    )
    transfer_cc.write_text(transfer_content)

    transfer_header = PW_ROOT / "pw_transfer" / "public" / "pw_transfer" / "transfer_thread.h"
    header_content = transfer_header.read_text()
    if "Status Poll(size_t budget = 1);" not in header_content:
        raise RuntimeError("cooperative Status Poll declaration was not applied")

    subprocess.run(
        ["git", "-C", str(PW_ROOT), "diff", "--check"],
        check=True,
    )
    print(f"Applied AVR protobuf compatibility delta for Pigweed {EXPECTED_PIGWEED_SHA}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"Pigweed AVR patch failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
