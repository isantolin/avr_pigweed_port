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
        '''#include "pw_span/span.h"
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
  // Process at most one queued event and any expired deadlines. Call from the
  // single owner loop after RPC packet dispatch and on every loop iteration.
  bool Poll();
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
    (void)Poll();
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
    PW_CHECK(!event_pending_, "Transfer event slot is already occupied");
    event_pending_ = true;
#else
    event_notification_.release();
#endif
  }

  bool TryWaitForEventToProcess() {
#if defined(PW_TRANSFER_COOPERATIVE)
    PW_CHECK(event_slot_available_,
             "Transfer event slot full; call Poll() after each event source");
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
  bool event_pending_ = false;
  bool event_slot_available_ = true;
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
    if "#if !defined(PW_TRANSFER_COOPERATIVE)\\nvoid TransferThread::Run()" not in transfer_content:
        run_end_marker = "\\n}\\n\\nstd::optional<chrono::SystemClock::time_point>"
        run_end = transfer_content.index(run_end_marker, run_start) + 3
        run_block = transfer_content[run_start:run_end]
        transfer_content = (
            transfer_content[:run_start]
            + "#if !defined(PW_TRANSFER_COOPERATIVE)\\n"
            + run_block
            + "#endif  // !PW_TRANSFER_COOPERATIVE\\n"
            + transfer_content[run_end:]
        )

    stream_start = transfer_content.index("void TransferThread::SetClientReadStream")
    stream_tail = transfer_content[stream_start:]
    old_wait = "  NotifyEvent();\n  WaitUntilEventIsProcessed();"
    new_wait = "  NotifyEvent();\n#if !defined(PW_TRANSFER_COOPERATIVE)\n  WaitUntilEventIsProcessed();\n#endif"
    wait_count = stream_tail.count(old_wait)
    if wait_count == 6:
        stream_tail = stream_tail.replace(old_wait, new_wait)
    elif stream_tail.count(new_wait) != 6:
        raise RuntimeError(
            f"expected six stream callback waits or six already-patched waits; found {wait_count}"
        )
    transfer_content = transfer_content[:stream_start] + stream_tail

    insert_marker = "std::optional<chrono::SystemClock::time_point>\nTransferThread::GetNextTransferTimeout() const {"
    if "bool TransferThread::Poll()" not in transfer_content:
        poll_code = '''#if defined(PW_TRANSFER_COOPERATIVE)
bool TransferThread::Poll() {
  bool did_work = false;
  if (event_pending_) {
    const bool is_terminating = next_event_.type == EventType::kTerminate;
    HandleEvent(next_event_);
    event_pending_ = false;
    event_slot_available_ = true;
    did_work = true;
    if (is_terminating) {
      return true;
    }
  }
  return HandleTimeouts() || did_work;
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
    transfer_cc.write_text(transfer_content)

    transfer_header = PW_ROOT / "pw_transfer" / "public" / "pw_transfer" / "transfer_thread.h"
    header_content = transfer_header.read_text()
    if "bool Poll();" not in header_content:
        raise RuntimeError("cooperative Poll declaration was not applied")

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
