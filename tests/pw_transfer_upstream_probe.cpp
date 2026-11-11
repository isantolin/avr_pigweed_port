#include <array>
#include <cstddef>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include "pw_bytes/span.h"
#include "pw_status/status.h"
#include "pw_transfer/client.h"
#include "pw_transfer/internal/event.h"
#include "pw_transfer/transfer.h"

namespace {

bool first_callback_called = false;
bool duplicate_rejected = false;

void OnFirstResourceStatus(
    pw::Status status,
    const pw::transfer::internal::ResourceStatus&) {
  first_callback_called = status == pw::Status::NotFound();
}

void OnDuplicateResourceStatus(
    pw::Status status,
    const pw::transfer::internal::ResourceStatus&) {
  duplicate_rejected = status == pw::Status::Unavailable();
}

void EmitResult(bool success) {
  const uint16_t ubrr =
      static_cast<uint16_t>((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = static_cast<uint8_t>(ubrr >> 8);
  UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR0B = (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  const char* marker = success ? "PW_TRANSFER_PROBE_PASS\r\n"
                               : "PW_TRANSFER_PROBE_FAIL\r\n";
  while (*marker != '\0') {
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = static_cast<uint8_t>(*marker++);
  }
  while (!(UCSR0A & (1 << TXC0))) {}
  cli();
  sleep_enable();
  sleep_cpu();
}

}  // namespace

int main() {
  std::array<std::byte, 128> chunk_buffer{};
  std::array<std::byte, 128> encode_buffer{};
  pw::transfer::Thread<1, 1> transfer_thread(
      pw::ByteSpan(chunk_buffer.data(), chunk_buffer.size()),
      pw::ByteSpan(encode_buffer.data(), encode_buffer.size()));
  pw::transfer::TransferService service(transfer_thread, 128);

  // The resource-status responder is a single staged upstream callback.
  // Verify the cooperative queue preserves that contract and honors budgets.
  first_callback_called = false;
  duplicate_rejected = false;
  transfer_thread.EnqueueResourceEvent(0x1234, OnFirstResourceStatus);
  transfer_thread.EnqueueResourceEvent(0x5678, OnDuplicateResourceStatus);

  bool success = duplicate_rejected;
  if (transfer_thread.Poll(0) != pw::Status::Unavailable()) success = false;
  if (!transfer_thread.Poll(1).ok()) success = false;
  if (!first_callback_called) success = false;
  if (!transfer_thread.Poll(2).ok()) success = false;

  EmitResult(success);
  return success ? 0 : 1;
}
