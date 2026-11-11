#include <array>
#include <cstddef>
#include <cstdint>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <utility>

#include "avr_uart_stream.h"
#include "pw_bytes/span.h"
#include "pw_function/function.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/raw/client_reader_writer.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_span/span.h"
#include "pw_status/status.h"
#include "pw_stream/stream.h"
#include "tests/rpc_probe.raw_rpc.pb.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
constexpr size_t kHdlcDecoderBufferSize = 128;

void EmitResult(uint8_t failure_code) {
  const uint16_t ubrr =
      static_cast<uint16_t>((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = static_cast<uint8_t>(ubrr >> 8);
  UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR0B = (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  char failure_marker[] = "PW_RPC_PROBE_FAIL_0\r\n";
  failure_marker[18] = static_cast<char>('0' + failure_code);
  const char* marker =
      failure_code == 0 ? "PW_RPC_PROBE_PASS\r\n" : failure_marker;
  while (*marker != '\0') {
    while (!(UCSR0A & (1 << UDRE0))) {}
    UDR0 = static_cast<uint8_t>(*marker++);
  }
  while (!(UCSR0A & (1 << TXC0))) {}
  cli();
  sleep_enable();
  sleep_cpu();
}

std::array<std::byte, 3> expected_request{};
bool unary_response_ok = false;
bool unary_status_ok = false;
size_t unary_response_size = 0;
uint8_t unary_response_first = 0;
uint8_t unary_response_last = 0;
bool bidi_response_ok = false;
bool bidi_response_called = false;
size_t bidi_response_size = 0;
uint8_t bidi_response_middle = 0;
bool bidi_server_write_ok = false;

void OnUnaryResponse(pw::ConstByteSpan response, pw::Status status) {
  unary_status_ok = status.ok();
  unary_response_size = response.size();
  if (!response.empty()) {
    unary_response_first = static_cast<uint8_t>(response[0]);
    unary_response_last = static_cast<uint8_t>(response[response.size() - 1]);
  }
  unary_response_ok = unary_status_ok &&
                      response.size() == expected_request.size() &&
                      response[0] == expected_request[0] &&
                      response[2] == expected_request[2];
}

void OnBidiResponse(pw::ConstByteSpan response) {
  bidi_response_called = true;
  bidi_response_size = response.size();
  if (response.size() > 1) {
    bidi_response_middle = static_cast<uint8_t>(response[1]);
  }
  bidi_response_ok = response.size() == expected_request.size() &&
                     response[1] == expected_request[1];
}

class ProbeService final
    : public ::avr::port::rpc::pw_rpc::raw::RpcProbe::Service<ProbeService> {
 public:
  void Unary(pw::ConstByteSpan request,
             pw::rpc::RawUnaryResponder& responder) {
    unary_called_ = true;
    unary_request_size_ = request.size();
    (void)responder.Finish(request, pw::OkStatus());
  }

  void Bidi(pw::rpc::RawServerReaderWriter& stream) {
    bidi_stream_ = std::move(stream);
    bidi_stream_.set_on_next([this](pw::ConstByteSpan payload) {
      bidi_server_received_ = true;
      bidi_server_write_ok = bidi_stream_.Write(payload).ok();
    });
  }

  bool unary_called() const { return unary_called_; }
  size_t unary_request_size() const { return unary_request_size_; }
  bool bidi_server_received() const { return bidi_server_received_; }

 private:
  pw::rpc::RawServerReaderWriter bidi_stream_;
  bool unary_called_ = false;
  size_t unary_request_size_ = 0;
  bool bidi_server_received_ = false;
};

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
}  // namespace

int main() {
  uint8_t failure_code = 0;

  // Keep the physical AVR UART/HDLC output in the link and verify that the
  // decoder accepts an incomplete frame without dispatching a packet.
  avr_port::AvrUartStream uart(115200);
  pw::hdlc::RpcChannelOutput hdlc_output(uart, kHdlcAddress, "avr-hdlc-probe");
  (void)hdlc_output;

  HdlcLoopbackWriter server_to_client_writer;
  pw::hdlc::RpcChannelOutput server_output(
      server_to_client_writer, kHdlcAddress, "avr-server-hdlc");
  std::array<pw::rpc::Channel, 1> server_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&server_output)};
  pw::rpc::Server server(pw::span<pw::rpc::Channel>(server_channels.data(),
                                                     server_channels.size()));

  ProbeService service;
  server.RegisterService(service);

  HdlcLoopbackWriter client_to_server_writer(server);
  pw::hdlc::RpcChannelOutput client_output(
      client_to_server_writer, kHdlcAddress, "avr-client-hdlc");
  std::array<pw::rpc::Channel, 1> client_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&client_output)};
  pw::rpc::Client client(pw::span<pw::rpc::Channel>(client_channels.data(),
                                                     client_channels.size()));
  server_to_client_writer.set_client(client);

  using RawProbeClient = ::avr::port::rpc::pw_rpc::raw::RpcProbe::Client;
  RawProbeClient raw_client(client, kRpcChannelId);

  const std::array<std::byte, 3> request = {
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  expected_request = request;
  auto unary_call = raw_client.Unary(request, OnUnaryResponse);
  (void)unary_call;
  if (!service.unary_called()) failure_code = 1;
  if (failure_code == 0 &&
      service.unary_request_size() != request.size()) failure_code = 2;
  if (failure_code == 0 && !unary_status_ok) failure_code = 7;
  if (failure_code == 0 &&
      unary_response_size != request.size()) failure_code = 8;
  if (failure_code == 0 &&
      (unary_response_first != static_cast<uint8_t>(request[0]) ||
       unary_response_last != static_cast<uint8_t>(request[2]))) failure_code = 9;

  auto bidi_call = raw_client.Bidi();
  bidi_call.set_on_next(OnBidiResponse);
  if (!bidi_call.Write(request).ok() && failure_code == 0) failure_code = 4;
  if (!service.bidi_server_received() && failure_code == 0) failure_code = 5;
  if (!bidi_server_write_ok && failure_code == 0) failure_code = 6;
  if (!bidi_response_called && failure_code == 0) failure_code = 7;
  if (bidi_response_size != request.size() && failure_code == 0) failure_code = 8;
  if (bidi_response_middle != static_cast<uint8_t>(request[1]) &&
      failure_code == 0) failure_code = 9;
  (void)bidi_call.Cancel();

  // The RPC calls above traversed both HDLC encoders and decoders.
  EmitResult(failure_code);
  return failure_code == 0 ? 0 : 1;
}
