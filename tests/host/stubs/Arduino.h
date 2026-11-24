#pragma once
#include <stdint.h>

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LSBFIRST 0
#define MSBFIRST 1
#define SPI_MODE0 0x00
#define SPI_MODE1 0x04
#define SPI_MODE2 0x08
#define SPI_MODE3 0x0C

namespace host_stub {
inline uint32_t g_millis = 0;
inline uint8_t g_pin_mode[64] = {};
inline uint8_t g_pin_val[64] = {};
inline int g_analog_val[64] = {};
}  // namespace host_stub

inline uint32_t millis() { return host_stub::g_millis; }
inline void pinMode(uint8_t pin, uint8_t mode) {
  if (pin < 64) host_stub::g_pin_mode[pin] = mode;
}
inline void digitalWrite(uint8_t pin, uint8_t val) {
  if (pin < 64) host_stub::g_pin_val[pin] = val;
}
inline int digitalRead(uint8_t pin) {
  return (pin < 64) ? host_stub::g_pin_val[pin] : LOW;
}
inline int analogRead(uint8_t pin) {
  return (pin < 64) ? host_stub::g_analog_val[pin] : 0;
}
