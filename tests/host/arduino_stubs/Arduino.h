#pragma once

#include <stddef.h>
#include <stdint.h>

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

namespace pw_avr_bazel_test {
inline uint32_t now_ms = 0;
inline uint8_t pin_mode[64] = {};
inline uint8_t pin_value[64] = {};
inline int analog_value[64] = {};
}  // namespace pw_avr_bazel_test

inline unsigned long millis() {
  return pw_avr_bazel_test::now_ms;
}

inline void pinMode(uint8_t pin, uint8_t mode) {
  if (pin < 64U) {
    pw_avr_bazel_test::pin_mode[pin] = mode;
  }
}

inline void digitalWrite(uint8_t pin, uint8_t value) {
  if (pin < 64U) {
    pw_avr_bazel_test::pin_value[pin] = value;
  }
}

inline int digitalRead(uint8_t pin) {
  return pin < 64U ? pw_avr_bazel_test::pin_value[pin] : LOW;
}

inline int analogRead(uint8_t pin) {
  return pin < 64U ? pw_avr_bazel_test::analog_value[pin] : 0;
}
