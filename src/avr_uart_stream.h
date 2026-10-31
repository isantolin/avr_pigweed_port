// File: src/avr_uart_stream.h
#pragma once

#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"

namespace avr_port {

class AvrUartStream final : public pw::stream::NonSeekableReaderWriter {
 public:
  explicit AvrUartStream(uint32_t baud_rate) {
    const uint16_t ubrr =
        static_cast<uint16_t>((F_CPU / (16UL * baud_rate)) - 1UL);
    UBRR0H = static_cast<uint8_t>(ubrr >> 8);
    UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);

    // Enable USART0 receiver and transmitter
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    // Frame format: 8 data bits, no parity, 1 stop bit (8N1)
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    for (const std::byte b : data) {
      while (!(UCSR0A & (1 << UDRE0))) {
        // Busy-wait until transmit buffer is ready
      }
      UDR0 = static_cast<uint8_t>(b);
    }
    return pw::OkStatus();
  }

  pw::StatusWithSize DoRead(pw::ByteSpan destination) override {
    if (destination.empty()) {
      return pw::StatusWithSize(0);
    }
    // Non-blocking read of the first available byte
    if (!(UCSR0A & (1 << RXC0))) {
      return pw::StatusWithSize::ResourceExhausted();
    }
    destination[0] = static_cast<std::byte>(UDR0);
    return pw::StatusWithSize(1);
  }
};

}  // namespace avr_port
