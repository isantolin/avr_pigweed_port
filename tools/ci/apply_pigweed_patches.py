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
    if constexpr (sizeof(double) != sizeof(uint64_t)) {
      (void)field_number;
      (void)value;
      return Status::Unimplemented();
    } else {
      uint64_t integral_value;
      std::memcpy(&integral_value, &value, sizeof(value));
      std::array<std::byte, sizeof(value)> data =
          bytes::CopyInOrder(endian::little, integral_value);
      return WriteFixed(field_number, data);
    }
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
