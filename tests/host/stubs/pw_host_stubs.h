#pragma once
// Host-only minimal Pigweed facade stubs used when third_party/pigweed submodule is absent.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace pw {

enum class StatusCode : uint8_t {
  kOk = 0,
  kInvalidArgument = 3,
  kDeadlineExceeded = 4,
  kFailedPrecondition = 9,
  kUnimplemented = 12,
};

class Status {
 public:
  constexpr Status() : code_(StatusCode::kOk) {}
  constexpr explicit Status(StatusCode c) : code_(c) {}
  static constexpr Status InvalidArgument() { return Status(StatusCode::kInvalidArgument); }
  static constexpr Status DeadlineExceeded() { return Status(StatusCode::kDeadlineExceeded); }
  static constexpr Status FailedPrecondition() { return Status(StatusCode::kFailedPrecondition); }
  static constexpr Status Unimplemented() { return Status(StatusCode::kUnimplemented); }
  constexpr bool ok() const { return code_ == StatusCode::kOk; }
  constexpr StatusCode code() const { return code_; }
  constexpr bool operator==(const Status& o) const { return code_ == o.code_; }
  constexpr bool operator!=(const Status& o) const { return code_ != o.code_; }

 private:
  StatusCode code_;
};

constexpr Status OkStatus() { return Status(); }

template <typename T>
class Result {
 public:
  Result(const T& v) : ok_(true), value_(v) {}
  Result(Status s) : ok_(false), status_(s) {}
  bool ok() const { return ok_; }
  Status status() const { return status_; }
  const T& value() const { return value_; }

 private:
  bool ok_;
  Status status_{};
  T value_{};
};

using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;

template <typename Sig>
using Function = std::function<Sig>;
template <typename Sig, size_t>
using InlineFunction = std::function<Sig>;

namespace chrono {
struct SystemClock {
  using time_point = uint32_t;
};
}  // namespace chrono

namespace spi {
enum class ClockPolarity { kActiveHigh, kActiveLow };
enum class ClockPhase { kRisingEdge, kFallingEdge };
enum class BitOrder { kMsbFirst, kLsbFirst };
struct BitsPerWord {
  uint8_t value;
  constexpr explicit BitsPerWord(uint8_t v) : value(v) {}
  constexpr uint8_t operator()() const { return value; }
};
struct Config {
  ClockPolarity polarity;
  ClockPhase phase;
  BitsPerWord bits_per_word;
  BitOrder bit_order;
};
class Initiator {
 public:
  virtual ~Initiator() = default;
  Status Configure(const Config& c) { return DoConfigure(c); }
  Status WriteRead(ConstByteSpan w, ByteSpan r) { return DoWriteRead(w, r); }
 protected:
  virtual Status DoConfigure(const Config&) = 0;
  virtual Status DoWriteRead(ConstByteSpan, ByteSpan) = 0;
};
class ChipSelector {
 public:
  virtual ~ChipSelector() = default;
  virtual Status SetActive(bool active) = 0;
};
}  // namespace spi

namespace digital_io {
enum class State { kInactive = 0, kActive = 1 };
class DigitalIn {
 public:
  virtual ~DigitalIn() = default;
  Status Enable() { return DoEnable(true); }
  Status Disable() { return DoEnable(false); }
  Result<State> GetState() { return DoGetState(); }
 protected:
  virtual Status DoEnable(bool) = 0;
  virtual Result<State> DoGetState() = 0;
};
class DigitalOut {
 public:
  virtual ~DigitalOut() = default;
  Status Enable() { return DoEnable(true); }
  Status Disable() { return DoEnable(false); }
  Status SetState(State s) { return DoSetState(s); }
 protected:
  virtual Status DoEnable(bool) = 0;
  virtual Status DoSetState(State) = 0;
};
class DigitalInOut {
 public:
  virtual ~DigitalInOut() = default;
  Status Enable() { return DoEnable(true); }
  Status Disable() { return DoEnable(false); }
  Result<State> GetState() { return DoGetState(); }
  Status SetState(State s) { return DoSetState(s); }
 protected:
  virtual Status DoEnable(bool) = 0;
  virtual Result<State> DoGetState() = 0;
  virtual Status DoSetState(State) = 0;
};
}  // namespace digital_io

namespace analog {
class AnalogInput {
 public:
  struct Limits {
    int32_t min;
    int32_t max;
  };
  virtual ~AnalogInput() = default;
  virtual Limits GetLimits() const = 0;
  virtual Result<int32_t> TryReadUntil(chrono::SystemClock::time_point) = 0;
};
}  // namespace analog

}  // namespace pw
