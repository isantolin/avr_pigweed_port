// File: src/arduino_stream.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"

#if defined(ARDUINO) || __has_include(<Stream.h>) || __has_include(<Arduino.h>)
#include <Arduino.h>
#include <Stream.h>

namespace avr_port {

/// Stream adapter that bridges any Arduino Stream / HardwareSerial (Serial, Serial1, etc.)
/// into Pigweed's pw::stream::NonSeekableReaderWriter interface.
///
/// Benefits in Arduino Yún Bridge 2:
/// - Adapts the selected Arduino Stream, including Mega 2560 hardware UARTs used for the MPU link.
/// - Non-blocking I/O leveraging Arduino Core's interrupt-driven circular RX/TX ring buffers.
/// - Immune to single-byte hardware FIFO overrun.
class ArduinoStream final : public pw::stream::NonSeekableReaderWriter {
 public:
  constexpr ArduinoStream() : stream_(nullptr) {}
  explicit constexpr ArduinoStream(::Stream& stream) : stream_(&stream) {}
  explicit constexpr ArduinoStream(::Stream* stream) : stream_(stream) {}

  void setStream(::Stream* stream) { stream_ = stream; }
  void setStream(::Stream& stream) { stream_ = &stream; }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    if (!stream_ || data.empty()) {
      return pw::OkStatus();
    }
    const size_t written = stream_->write(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
    if (written != data.size()) {
      return pw::Status::ResourceExhausted();
    }
    return pw::OkStatus();
  }

  pw::StatusWithSize DoRead(pw::ByteSpan destination) override {
    if (!stream_ || destination.empty()) {
      return pw::StatusWithSize(0);
    }
    size_t bytes_read = 0;
    while (bytes_read < destination.size() && stream_->available() > 0) {
      const int c = stream_->read();
      if (c < 0) {
        break;
      }
      destination[bytes_read++] = static_cast<std::byte>(c);
    }
    if (bytes_read == 0) {
      return pw::StatusWithSize::ResourceExhausted();
    }
    return pw::StatusWithSize(bytes_read);
  }

  ::Stream* stream_ = nullptr;
};

}  // namespace avr_port

#endif  // ARDUINO
