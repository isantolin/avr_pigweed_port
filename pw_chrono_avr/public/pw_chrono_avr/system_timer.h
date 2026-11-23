// File: pw_chrono_avr/public/pw_chrono_avr/system_timer.h
#pragma once

#include <stdint.h>

#include "pw_chrono_avr/system_clock_avr.h"
#include "pw_function/function.h"

namespace pw::chrono::avr {

/// Zero-heap cooperative timer for bare-metal AVR / Arduino loops.
/// Invokes at most one armed callback per instance when Poll() observes
/// that pw_chrono_avr_GetMillis() has reached the scheduled deadline
/// (wraparound-safe 32-bit modular arithmetic).
class CooperativeSystemTimer {
 public:
  using Callback = pw::InlineFunction<void(uint32_t now_ms), 8>;

  CooperativeSystemTimer() = default;
  ~CooperativeSystemTimer() { Cancel(); }

  CooperativeSystemTimer(const CooperativeSystemTimer&) = delete;
  CooperativeSystemTimer& operator=(const CooperativeSystemTimer&) = delete;

  /// Schedules `callback` to run after `delay_ms` milliseconds.
  /// Replaces any previously armed deadline.
  void InvokeAfter(uint32_t delay_ms, Callback callback);

  /// Schedules `callback` to run at `deadline_ms` (from pw_chrono_avr_GetMillis).
  /// Replaces any previously armed deadline.
  void InvokeAt(uint32_t deadline_ms, Callback callback);

  /// Cancels any pending invocation.
  void Cancel();

  /// Returns true if a deadline is currently armed.
  bool IsPending() const { return armed_; }

  /// Polls the system clock and fires the callback if the deadline expired.
  /// Returns true if the callback was invoked during this Poll() call.
  bool Poll();

 private:
  Callback callback_{};
  uint32_t deadline_ms_{0};
  bool armed_{false};
};

}  // namespace pw::chrono::avr
