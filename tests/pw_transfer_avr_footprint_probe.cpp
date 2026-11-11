#include <array>
#include <cstddef>
#include <cstdint>
#include <avr/io.h>

#if defined(__AVR_ATmega32U4__)
#define PW_TRANSFER_UART_UBRRH UBRR1H
#define PW_TRANSFER_UART_UBRRL UBRR1L
#define PW_TRANSFER_UART_UCSRB UCSR1B
#define PW_TRANSFER_UART_TXEN TXEN1
#define PW_TRANSFER_UART_UCSRC UCSR1C
#define PW_TRANSFER_UART_UCSZ1 UCSZ11
#define PW_TRANSFER_UART_UCSZ0 UCSZ10
#define PW_TRANSFER_UART_UCSRA UCSR1A
#define PW_TRANSFER_UART_UDRE UDRE1
#define PW_TRANSFER_UART_UDR UDR1
#define PW_TRANSFER_UART_TXC TXC1
#else
#define PW_TRANSFER_UART_UBRRH UBRR0H
#define PW_TRANSFER_UART_UBRRL UBRR0L
#define PW_TRANSFER_UART_UCSRB UCSR0B
#define PW_TRANSFER_UART_TXEN TXEN0
#define PW_TRANSFER_UART_UCSRC UCSR0C
#define PW_TRANSFER_UART_UCSZ1 UCSZ01
#define PW_TRANSFER_UART_UCSZ0 UCSZ00
#define PW_TRANSFER_UART_UCSRA UCSR0A
#define PW_TRANSFER_UART_UDRE UDRE0
#define PW_TRANSFER_UART_UDR UDR0
#define PW_TRANSFER_UART_TXC TXC0
#endif

#include "PigweedAVR.h"
#include "pw_bytes/span.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"
#include "pw_stream/memory_stream.h"
#include "pw_stream/stream.h"
#include "pw_transfer/client.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/transfer.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
volatile bool g_start_transfer = false;

class NullWriter final : public pw::stream::NonSeekableWriter {
 private:
  pw::Status DoWrite(pw::ConstByteSpan) override { return pw::OkStatus(); }
};

void EmitResult() {
  const uint16_t ubrr =
      static_cast<uint16_t>((F_CPU / (16UL * 115200UL)) - 1UL);
#if defined(PW_TRANSFER_UART_UBRRH)
  PW_TRANSFER_UART_UBRRH = static_cast<uint8_t>(ubrr >> 8);
  PW_TRANSFER_UART_UBRRL = static_cast<uint8_t>(ubrr & 0xFF);
  PW_TRANSFER_UART_UCSRB = (1 << PW_TRANSFER_UART_TXEN);
  PW_TRANSFER_UART_UCSRC = (1 << PW_TRANSFER_UART_UCSZ1) | (1 << PW_TRANSFER_UART_UCSZ0);
  const char marker[] = "PW_TRANSFER_FOOTPRINT_PASS\r\n";
  for (size_t i = 0; marker[i] != '\0'; ++i) {
    while (!(PW_TRANSFER_UART_UCSRA & (1 << PW_TRANSFER_UART_UDRE))) {}
    PW_TRANSFER_UART_UDR = static_cast<uint8_t>(marker[i]);
  }
  while (!(PW_TRANSFER_UART_UCSRA & (1 << PW_TRANSFER_UART_TXC))) {}
#elif defined(UBRR1H)
  UBRR1H = static_cast<uint8_t>(ubrr >> 8);
  UBRR1L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR1B = (1 << TXEN1);
  UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);
  const char marker[] = "PW_TRANSFER_FOOTPRINT_PASS\r\n";
  for (size_t i = 0; marker[i] != '\0'; ++i) {
    while (!(UCSR1A & (1 << UDRE1))) {}
    UDR1 = static_cast<uint8_t>(marker[i]);
  }
  while (!(UCSR1A & (1 << TXC1))) {}
#else
#error "No supported USART register bank found for footprint probe"
#endif
}

}  // namespace

int main() {
  static std::array<std::byte, PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE> chunk_storage{};
  static std::array<std::byte, 128> encode_storage{};
  static pw::transfer::Thread<1, 1> transfer_thread{
      pw::ByteSpan(chunk_storage), pw::ByteSpan(encode_storage)};

  // The production Bridge owns one RPC channel and uses the same transfer
  // thread for its client and server objects, with role arbitration preventing
  // simultaneous client/server transfers.
  static NullWriter uart_writer;
  static pw::hdlc::RpcChannelOutput hdlc_output(
      uart_writer, kHdlcAddress, "pw-transfer-atmega32u4-footprint");
  static std::array<pw::rpc::Channel, 1> channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&hdlc_output)};
  static pw::rpc::Server rpc_server(
      pw::span<pw::rpc::Channel>(channels.data(), channels.size()));
  static pw::rpc::Client rpc_client(
      pw::span<pw::rpc::Channel>(channels.data(), channels.size()));

  static pw::transfer::TransferService transfer_service(transfer_thread, 256);
  rpc_server.RegisterService(transfer_service);
  static pw::transfer::Client transfer_client(
      rpc_client, kRpcChannelId, transfer_thread, 256);

  std::array<std::byte, 32> transfer_data{};
  std::array<std::byte, 64> transfer_output_buffer{};
  pw::stream::MemoryReader transfer_reader{pw::ConstByteSpan(transfer_data)};
  pw::stream::MemoryWriter transfer_writer{pw::ByteSpan(transfer_output_buffer)};
  pw::transfer::ReadOnlyHandler read_handler(1, transfer_reader);
  pw::transfer::WriteOnlyHandler write_handler(2, transfer_writer);
  (void)transfer_service.RegisterHandler(read_handler);
  (void)transfer_service.RegisterHandler(write_handler);

  // Keep the real client Read/Write paths in the linked image for a realistic
  // application footprint without starting a transfer in this probe.
  if (g_start_transfer) {
    (void)transfer_client.Read(
        1, transfer_writer,
        [](pw::Status) {});
    (void)transfer_client.Write(
        2, transfer_reader,
        [](pw::Status) {});
  }

  (void)transfer_thread.Poll(1);
  EmitResult();
  return 0;
}
