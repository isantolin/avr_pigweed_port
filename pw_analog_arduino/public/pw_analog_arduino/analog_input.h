// File: pw_analog_arduino/public/pw_analog_arduino/analog_input.h
#pragma once

#include <stdint.h>

#if defined(ARDUINO) || __has_include(<Arduino.h>)
#include <Arduino.h>
#endif

#include "pw_analog/analog_input.h"
#include "pw_chrono/system_clock.h"
#include "pw_result/result.h"

namespace pw::analog {

/// Arduino AnalogInput implementation over ::analogRead().
class ArduinoAnalogInput final : public AnalogInput {
 public:
  explicit constexpr ArduinoAnalogInput(uint8_t pin,
                                        int32_t max_sample = 1023)
      : pin_(pin), max_sample_(max_sample) {}

  Limits GetLimits() const override {
    return Limits{
        .min = 0,
        .max = max_sample_,
    };
  }

 private:
  Result<int32_t> TryReadUntil(
      pw::chrono::SystemClock::time_point /*deadline*/) override {
#if defined(ARDUINO) || __has_include(<Arduino.h>)
    return static_cast<int32_t>(::analogRead(pin_));
#else
    return Status::Unimplemented();
#endif
  }

  uint8_t pin_;
  int32_t max_sample_;
};

}  // namespace pw::analog
