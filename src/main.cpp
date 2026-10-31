// File: src/main.cpp
#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>
#include <util/delay.h>

#include "avr_uart_stream.h"
#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_hdlc/encoder.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_varint/varint.h"

namespace {

constexpr uint8_t kTelemetryHdlcAddress = 1;

void InitAdc() {
  // AVcc reference (5V) and default ADC0 channel selected
  ADMUX = (1 << REFS0);
  // Enable ADC with prescaler of 128 (16 MHz / 128 = 125 kHz)
  ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1) | (1 << ADPS0);
}

// Reads channel ADC0 and returns a pw::Result<uint16_t> without using exceptions
pw::Result<uint16_t> ReadAdcChannel0() {
  ADCSRA |= (1 << ADSC);
  while (ADCSRA & (1 << ADSC)) {
    // Wait for conversion completion
  }
  const uint16_t value = ADC;
  if (value > 1023U) {
    return pw::Status::OutOfRange();
  }
  return value;
}

// Packs sequence number and ADC reading using pw::varint
pw::Status BuildTelemetryPayload(uint16_t sequence,
                                 pw::Vector<std::byte, 16>& payload) {
  payload.clear();

  uint16_t adc_sample{};
  PW_TRY_ASSIGN(adc_sample, ReadAdcChannel0());

  std::byte varint_buf[pw::varint::kMaxVarint32SizeBytes];

  // Encode sequence number
  const size_t seq_len = pw::varint::Encode(sequence, varint_buf);
  for (size_t i = 0; i < seq_len; ++i) {
    if (payload.full()) {
      return pw::Status::ResourceExhausted();
    }
    payload.push_back(varint_buf[i]);
  }

  // Encode ADC value
  const size_t adc_len = pw::varint::Encode(adc_sample, varint_buf);
  for (size_t i = 0; i < adc_len; ++i) {
    if (payload.full()) {
      return pw::Status::ResourceExhausted();
    }
    payload.push_back(varint_buf[i]);
  }

  return pw::OkStatus();
}

}  // namespace

int main() {
  // Configure pin PB5 (built-in LED on Arduino Uno/Nano) as output
  DDRB |= (1 << DDB5);
  InitAdc();

  avr_port::AvrUartStream serial_stream(115200);
  pw::Vector<std::byte, 16> packet_payload;
  uint16_t sequence_counter = 0;

  for (;;) {
    if (BuildTelemetryPayload(sequence_counter++, packet_payload).ok()) {
      const pw::ConstByteSpan payload_span(packet_payload.data(),
                                           packet_payload.size());

      // Send HDLC frame with address 1 and CRC-32 checksum
      if (pw::hdlc::WriteUIFrame(
              kTelemetryHdlcAddress, payload_span, serial_stream)
              .ok()) {
        PORTB ^= (1 << PORTB5);  // Toggle LED on successful transmission
      }
    }
    _delay_ms(500);
  }

  return 0;
}
