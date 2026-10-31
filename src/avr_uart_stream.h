// File: src/avr_uart_stream.h
#pragma once

#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>

#include "arduino_stream.h"
#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"
#include "pw_stream/stream.h"

namespace avr_port {

enum class UartPort : uint8_t {
  kUsart0 = 0,
#if defined(UDR1) || defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega2560__)
  kUsart1 = 1,
#endif
};

/// Hardware UART stream for bare-metal AVR and simavr simulation.
/// Supports both USART0 (standard) and USART1 (ATmega32u4/Mega2560 communicating with OpenWrt).
class AvrUartStream final : public pw::stream::NonSeekableReaderWriter {
 public:
  explicit AvrUartStream(uint32_t baud_rate, UartPort port = UartPort::kUsart0)
      : port_(port) {
    const uint16_t ubrr =
        static_cast<uint16_t>((F_CPU / (16UL * baud_rate)) - 1UL);

#if defined(UDR1) || defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega2560__)
    if (port_ == UartPort::kUsart1) {
      UBRR1H = static_cast<uint8_t>(ubrr >> 8);
      UBRR1L = static_cast<uint8_t>(ubrr & 0xFF);
      UCSR1B = (1 << RXEN1) | (1 << TXEN1);
      UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);
      return;
    }
#endif

#if defined(UDR0)
    UBRR0H = static_cast<uint8_t>(ubrr >> 8);
    UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
#endif
  }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    for (const std::byte b : data) {
#if defined(UDR1) || defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega2560__)
      if (port_ == UartPort::kUsart1) {
        while (!(UCSR1A & (1 << UDRE1))) {}
        UDR1 = static_cast<uint8_t>(b);
        continue;
      }
#endif
#if defined(UDR0)
      while (!(UCSR0A & (1 << UDRE0))) {}
      UDR0 = static_cast<uint8_t>(b);
#endif
    }
    return pw::OkStatus();
  }

  pw::StatusWithSize DoRead(pw::ByteSpan destination) override {
    if (destination.empty()) {
      return pw::StatusWithSize(0);
    }
#if defined(UDR1) || defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega2560__)
    if (port_ == UartPort::kUsart1) {
      if (!(UCSR1A & (1 << RXC1))) {
        return pw::StatusWithSize::ResourceExhausted();
      }
      destination[0] = static_cast<std::byte>(UDR1);
      return pw::StatusWithSize(1);
    }
#endif
#if defined(UDR0)
    if (!(UCSR0A & (1 << RXC0))) {
      return pw::StatusWithSize::ResourceExhausted();
    }
    destination[0] = static_cast<std::byte>(UDR0);
    return pw::StatusWithSize(1);
#else
    return pw::StatusWithSize::ResourceExhausted();
#endif
  }

  UartPort port_;
};

}  // namespace avr_port
