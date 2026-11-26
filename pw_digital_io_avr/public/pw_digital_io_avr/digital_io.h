// File: pw_digital_io_avr/public/pw_digital_io_avr/digital_io.h
#pragma once

#include <stdint.h>

#if __has_include(<Arduino.h>)
#include <Arduino.h>
#endif

#include "pw_digital_io/digital_io.h"
#include "pw_result/result.h"
#include "pw_status/status.h"

namespace pw::digital_io {

/// Arduino DigitalIn implementation over ::pinMode / ::digitalRead.
class ArduinoDigitalIn final : public DigitalIn {
 public:
  explicit constexpr ArduinoDigitalIn(uint8_t pin,
                                     bool pullup = false,
                                     bool configure_pin = true)
      : pin_(pin),
        pullup_(pullup),
        configure_pin_(configure_pin),
        enabled_(false) {}

 private:
  Status DoEnable(bool enable) override;
  Result<State> DoGetState() override;

  uint8_t pin_;
  bool pullup_;
  bool configure_pin_;
  bool enabled_;
};

/// Arduino DigitalOut implementation over ::pinMode / ::digitalWrite.
class ArduinoDigitalOut final : public DigitalOut {
 public:
  explicit constexpr ArduinoDigitalOut(uint8_t pin,
                                       State initial_state = State::kInactive)
      : pin_(pin), initial_state_(initial_state), enabled_(false) {}

 private:
  Status DoEnable(bool enable) override;
  Status DoSetState(State state) override;

  uint8_t pin_;
  State initial_state_;
  bool enabled_;
};

/// Arduino DigitalInOut implementation over ::pinMode / ::digitalRead / ::digitalWrite.
class ArduinoDigitalInOut final : public DigitalInOut {
 public:
  explicit constexpr ArduinoDigitalInOut(
      uint8_t pin,
      bool pullup = false,
      State initial_state = State::kInactive,
      bool configure_pin = true)
      : pin_(pin),
        pullup_(pullup),
        initial_state_(initial_state),
        configure_pin_(configure_pin),
        enabled_(false) {}

 private:
  Status DoEnable(bool enable) override;
  Result<State> DoGetState() override;
  Status DoSetState(State state) override;

  uint8_t pin_;
  bool pullup_;
  State initial_state_;
  bool configure_pin_;
  bool enabled_;
};

}  // namespace pw::digital_io
