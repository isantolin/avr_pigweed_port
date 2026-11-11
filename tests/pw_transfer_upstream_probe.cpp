#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#include "pw_bytes/span.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"
#include "pw_stream/memory_stream.h"
#include "pw_transfer/client.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/internal/event.h"
#include "pw_transfer/transfer.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
constexpr uint32_t kReadResourceId = 0x1234;
constexpr uint32_t kWriteResourceId = 0x5678;
constexpr size_t kHdlcDecoderBufferSize = 128;

bool first_callback_called = false;
bool duplicate_rejected = false;
bool read_complete = false;
bool write_complete = false;
bool reverse_write_complete = false;
pw::Status read_completion_status = pw::Status::Unknown();
pw::Status write_completion_status = pw::Status::Unknown();
pw::Status reverse_write_completion_status = pw::Status::Unknown();

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

void OnReadComplete(pw::Status status) {
  read_completion_status = status;
  read_complete = true;
}

void OnWriteComplete(pw::Status status) {
  write_completion_status = status;
  write_complete = true;
}

void OnReverseWriteComplete(pw::Status status) {
  reverse_write_completion_status = status;
  reverse_write_complete = true;
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

class HdlcLoopbackWriter final : public pw::stream::NonSeekableWriter {
 public:
  explicit HdlcLoopbackWriter(pw::rpc::Server& server)
      : server_(&server), decoder_(pw::ByteSpan(decoder_storage_)) {}

  HdlcLoopbackWriter()
      : decoder_(pw::ByteSpan(decoder_storage_)) {}

  void set_client(pw::rpc::Client& client) { client_ = &client; }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    for (std::byte byte : data) {
      auto frame = decoder_.Process(byte);
      if (frame.status() == pw::Status::Unavailable()) continue;
      if (!frame.ok()) return frame.status();
      if (frame->address() != kHdlcAddress) continue;
      if (server_ != nullptr) {
        pw::Status status = server_->ProcessPacket(frame->data());
        if (!status.ok()) return status;
      } else if (client_ != nullptr) {
        pw::Status status = client_->ProcessPacket(frame->data());
        if (!status.ok()) return status;
      } else {
        return pw::Status::FailedPrecondition();
      }
    }
    return pw::OkStatus();
  }

  pw::rpc::Server* server_ = nullptr;
  pw::rpc::Client* client_ = nullptr;
  std::array<std::byte, kHdlcDecoderBufferSize> decoder_storage_{};
  pw::hdlc::Decoder decoder_;
};

bool PumpTransfers(pw::transfer::Thread<1, 1>& client_thread,
                   pw::transfer::Thread<1, 1>& server_thread,
                   bool& complete) {
  for (size_t i = 0; i < 256 && !complete; ++i) {
    const pw::Status client_status = client_thread.Poll(2);
    const pw::Status server_status = server_thread.Poll(2);
    if (client_status == pw::Status::ResourceExhausted() ||
        server_status == pw::Status::ResourceExhausted()) {
      return false;
    }
    if (!client_status.ok() &&
        client_status != pw::Status::Unavailable()) {
      return false;
    }
    if (!server_status.ok() &&
        server_status != pw::Status::Unavailable()) {
      return false;
    }
  }
  return complete;
}

}  // namespace

int main() {
  static std::array<std::byte, 128> client_chunk{};
  static std::array<std::byte, 128> client_encode{};
  static std::array<std::byte, 128> server_chunk{};
  static std::array<std::byte, 128> server_encode{};
  static pw::transfer::Thread<1, 1> client_thread{
      pw::ByteSpan(client_chunk), pw::ByteSpan(client_encode)};
  static pw::transfer::Thread<1, 1> server_thread{
      pw::ByteSpan(server_chunk), pw::ByteSpan(server_encode)};

  // The resource-status responder is a single staged upstream callback.
  // Verify the cooperative queue preserves that contract and honors budgets.
  first_callback_called = false;
  duplicate_rejected = false;
  client_thread.EnqueueResourceEvent(0x9999, OnFirstResourceStatus);
  client_thread.EnqueueResourceEvent(0x999A, OnDuplicateResourceStatus);

  bool success = duplicate_rejected;
  if (client_thread.Poll(0) != pw::Status::Unavailable()) success = false;
  if (!client_thread.Poll(1).ok()) success = false;
  if (!first_callback_called) success = false;

  static HdlcLoopbackWriter server_to_client_writer;
  static pw::hdlc::RpcChannelOutput server_output(
      server_to_client_writer, kHdlcAddress, "pw-transfer-server-hdlc");
  static std::array<pw::rpc::Channel, 1> server_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&server_output)};
  static pw::rpc::Server server(pw::span<pw::rpc::Channel>(
      server_channels.data(), server_channels.size()));

  static HdlcLoopbackWriter client_to_server_writer(server);
  static pw::hdlc::RpcChannelOutput client_output(
      client_to_server_writer, kHdlcAddress, "pw-transfer-client-hdlc");
  static std::array<pw::rpc::Channel, 1> client_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&client_output)};
  static pw::rpc::Client rpc_client(pw::span<pw::rpc::Channel>(
      client_channels.data(), client_channels.size()));
  server_to_client_writer.set_client(rpc_client);

  static pw::transfer::TransferService transfer_service(server_thread, 256);
  server.RegisterService(transfer_service);
  static pw::transfer::Client transfer_client(
      rpc_client, kRpcChannelId, client_thread, 256);

  std::array<std::byte, 32> read_data{};
  for (size_t i = 0; i < read_data.size(); ++i) {
    read_data[i] = static_cast<std::byte>(i + 1);
  }
  pw::stream::MemoryReader read_source{pw::ConstByteSpan(read_data)};
  pw::transfer::ReadOnlyHandler read_handler(kReadResourceId, read_source);
  if (!transfer_service.RegisterHandler(read_handler)) success = false;

  std::array<std::byte, 64> read_output_buffer{};
  pw::stream::MemoryWriter read_output{pw::ByteSpan(read_output_buffer)};
  read_complete = false;
  auto read_result =
      transfer_client.Read(kReadResourceId, read_output, OnReadComplete);
  if (!read_result.ok()) {
    success = false;
  } else if (!PumpTransfers(client_thread, server_thread, read_complete)) {
    success = false;
  }
  if (!read_completion_status.ok() ||
      read_output.bytes_written() != read_data.size() ||
      std::memcmp(read_output.data(), read_data.data(), read_data.size()) != 0) {
    success = false;
  }

  std::array<std::byte, 32> write_data{};
  for (size_t i = 0; i < write_data.size(); ++i) {
    write_data[i] = static_cast<std::byte>(0xA0 + i);
  }
  std::array<std::byte, 64> write_sink_buffer{};
  pw::stream::MemoryWriter write_sink{pw::ByteSpan(write_sink_buffer)};
  pw::transfer::WriteOnlyHandler write_handler(kWriteResourceId, write_sink);
  if (!transfer_service.RegisterHandler(write_handler)) success = false;

  pw::stream::MemoryReader write_source{pw::ConstByteSpan(write_data)};
  write_complete = false;
  auto write_result =
      transfer_client.Write(kWriteResourceId, write_source, OnWriteComplete);
  if (!write_result.ok()) {
    success = false;
  } else if (!PumpTransfers(client_thread, server_thread, write_complete)) {
    success = false;
  }
  if (!write_completion_status.ok() ||
      write_sink.bytes_written() != write_data.size() ||
      std::memcmp(write_sink.data(), write_data.data(), write_data.size()) != 0) {
    success = false;
  }

  // Reuse the same two TransferThread instances with their roles reversed.
  // The first endpoint now serves transfers and the second endpoint is client.
  static HdlcLoopbackWriter reverse_server_to_client_writer;
  static pw::hdlc::RpcChannelOutput reverse_server_output(
      reverse_server_to_client_writer, kHdlcAddress, "pw-transfer-reverse-server");
  static std::array<pw::rpc::Channel, 1> reverse_server_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&reverse_server_output)};
  static pw::rpc::Server reverse_server(pw::span<pw::rpc::Channel>(
      reverse_server_channels.data(), reverse_server_channels.size()));

  static HdlcLoopbackWriter reverse_client_to_server_writer(reverse_server);
  static pw::hdlc::RpcChannelOutput reverse_client_output(
      reverse_client_to_server_writer, kHdlcAddress, "pw-transfer-reverse-client");
  static std::array<pw::rpc::Channel, 1> reverse_client_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&reverse_client_output)};
  static pw::rpc::Client reverse_rpc_client(pw::span<pw::rpc::Channel>(
      reverse_client_channels.data(), reverse_client_channels.size()));
  reverse_server_to_client_writer.set_client(reverse_rpc_client);

  static pw::transfer::TransferService reverse_transfer_service(
      client_thread, 256);
  reverse_server.RegisterService(reverse_transfer_service);
  static pw::transfer::Client reverse_transfer_client(
      reverse_rpc_client, kRpcChannelId, server_thread, 256);

  std::array<std::byte, 32> reverse_data{};
  for (size_t i = 0; i < reverse_data.size(); ++i) {
    reverse_data[i] = static_cast<std::byte>(0x40 + i);
  }
  std::array<std::byte, 64> reverse_sink_buffer{};
  pw::stream::MemoryWriter reverse_sink{pw::ByteSpan(reverse_sink_buffer)};
  pw::transfer::WriteOnlyHandler reverse_handler(kWriteResourceId,
                                                  reverse_sink);
  if (!reverse_transfer_service.RegisterHandler(reverse_handler)) {
    success = false;
  }
  pw::stream::MemoryReader reverse_source{pw::ConstByteSpan(reverse_data)};
  reverse_write_complete = false;
  auto reverse_result = reverse_transfer_client.Write(
      kWriteResourceId, reverse_source, OnReverseWriteComplete);
  if (!reverse_result.ok()) {
    success = false;
  } else if (!PumpTransfers(server_thread, client_thread,
                            reverse_write_complete)) {
    success = false;
  }
  if (!reverse_write_completion_status.ok() ||
      reverse_sink.bytes_written() != reverse_data.size() ||
      std::memcmp(reverse_sink.data(), reverse_data.data(),
                  reverse_data.size()) != 0) {
    success = false;
  }

  EmitResult(success);
  return success ? 0 : 1;
}
