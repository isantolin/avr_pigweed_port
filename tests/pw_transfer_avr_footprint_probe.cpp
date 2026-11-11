#include <array>
#include <cstddef>
#include <cstdint>
#include <avr/io.h>

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
  UBRR0H = static_cast<uint8_t>(ubrr >> 8);
  UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR0B = (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  const char marker[] = "PW_TRANSFER_FOOTPRINT_PASS\r\n";
  for (size_t i = 0; marker[i] != '\0'; ++i) {
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = static_cast<uint8_t>(marker[i]);
  }
  while (!(UCSR0A & (1 << TXC0))) {}
}

}  // namespace

int main() {
  static std::array<std::byte, 128> chunk_storage{};
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
