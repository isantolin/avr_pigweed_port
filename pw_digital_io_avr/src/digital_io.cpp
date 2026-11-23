// File: pw_digital_io_avr/src/digital_io.cpp
#include "pw_digital_io_avr/digital_io.h"

#if defined(ARDUINO) || __has_include(<Arduino.h>)

namespace pw::digital_io {

Status ArduinoDigitalIn::DoEnable(bool enable) {
  if (enable) {
    ::pinMode(pin_, pullup_ ? INPUT_PULLUP : INPUT);
    enabled_ = true;
  } else {
    ::pinMode(pin_, INPUT);
    enabled_ = false;
  }
  return OkStatus();
}

Result<State> ArduinoDigitalIn::DoGetState() {
  if (!enabled_) {
    return Status::FailedPrecondition();
  }
  return (::digitalRead(pin_) == HIGH) ? State::kActive : State::kInactive;
}

Status ArduinoDigitalOut::DoEnable(bool enable) {
  if (enable) {
    if (!enabled_) {
      ::pinMode(pin_, OUTPUT);
      ::digitalWrite(pin_, (initial_state_ == State::kActive) ? HIGH : LOW);
      enabled_ = true;
    }
  } else {
    ::pinMode(pin_, INPUT);
    enabled_ = false;
  }
  return OkStatus();
}

Status ArduinoDigitalOut::DoSetState(State state) {
  if (!enabled_) {
    return Status::FailedPrecondition();
  }
  ::digitalWrite(pin_, (state == State::kActive) ? HIGH : LOW);
  return OkStatus();
}

Status ArduinoDigitalInOut::DoEnable(bool enable) {
  if (enable) {
    if (!enabled_) {
      ::pinMode(pin_, OUTPUT);
      ::digitalWrite(pin_, (initial_state_ == State::kActive) ? HIGH : LOW);
      enabled_ = true;
    }
  } else {
    ::pinMode(pin_, pullup_ ? INPUT_PULLUP : INPUT);
    enabled_ = false;
  }
  return OkStatus();
}

Result<State> ArduinoDigitalInOut::DoGetState() {
  if (!enabled_) {
    return Status::FailedPrecondition();
  }
  return (::digitalRead(pin_) == HIGH) ? State::kActive : State::kInactive;
}

Status ArduinoDigitalInOut::DoSetState(State state) {
  if (!enabled_) {
    return Status::FailedPrecondition();
  }
  ::pinMode(pin_, OUTPUT);
  ::digitalWrite(pin_, (state == State::kActive) ? HIGH : LOW);
  return OkStatus();
}

}  // namespace pw::digital_io

#endif
