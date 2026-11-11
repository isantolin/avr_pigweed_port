#include <array>
#include <cstddef>
#include <cstdint>
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
#include "tests/rpc_probe.raw_rpc.pb.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
constexpr size_t kHdlcDecoderBufferSize = 128;

std::array<std::byte, 3> expected_request{};
bool unary_response_ok = false;
bool bidi_response_ok = false;

void OnUnaryResponse(pw::ConstByteSpan response, pw::Status status) {
  unary_response_ok = status.ok() &&
                      response.size() == expected_request.size() &&
                      response[0] == expected_request[0] &&
                      response[2] == expected_request[2];
}

void OnBidiResponse(pw::ConstByteSpan response) {
  bidi_response_ok = response.size() == expected_request.size() &&
                     response[1] == expected_request[1];
}

class HdlcRpcIngress {
 public:
  HdlcRpcIngress(pw::rpc::Server& server, pw::ByteSpan buffer)
      : server_(server), decoder_(buffer) {}

  pw::Status ProcessByte(std::byte byte) {
    auto frame = decoder_.Process(byte);
    if (frame.status() == pw::Status::Unavailable()) {
      return pw::Status::Unavailable();
    }
    if (!frame.ok()) {
      return frame.status();
    }
    if (frame->address() != kHdlcAddress) {
      return pw::Status::NotFound();
    }
    return server_.ProcessPacket(frame->data());
  }

 private:
  pw::rpc::Server& server_;
  pw::hdlc::Decoder decoder_;
};

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
      (void)bidi_stream_.Write(payload);
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

class ClientToServerOutput final : public pw::rpc::ChannelOutput {
 public:
  explicit ClientToServerOutput(pw::rpc::Server& server)
      : pw::rpc::ChannelOutput("avr-rpc-client-to-server"), server_(server) {}

  pw::Status Send(pw::span<const std::byte> packet) override {
    return server_.ProcessPacket(packet);
  }

 private:
  pw::rpc::Server& server_;
};

class ServerToClientOutput final : public pw::rpc::ChannelOutput {
 public:
  ServerToClientOutput() : pw::rpc::ChannelOutput("avr-rpc-server-to-client") {}

  void set_client(pw::rpc::Client& client) { client_ = &client; }

  pw::Status Send(pw::span<const std::byte> packet) override {
    return client_ == nullptr ? pw::Status::Unavailable()
                              : client_->ProcessPacket(packet);
  }

 private:
  pw::rpc::Client* client_ = nullptr;
};

}  // namespace

int main() {
  // Keep the physical AVR UART/HDLC output in the link and verify that the
  // decoder accepts an incomplete frame without dispatching a packet.
  avr_port::AvrUartStream uart(115200);
  pw::hdlc::RpcChannelOutput hdlc_output(uart, kHdlcAddress, "avr-hdlc-probe");
  (void)hdlc_output;

  std::array<std::byte, kHdlcDecoderBufferSize> decoder_storage{};
  pw::rpc::Server* server_ptr = nullptr;
  HdlcRpcIngress* ingress_ptr = nullptr;

  ServerToClientOutput server_output;
  std::array<pw::rpc::Channel, 1> server_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&server_output)};
  pw::rpc::Server server(pw::span<pw::rpc::Channel>(server_channels.data(),
                                                     server_channels.size()));
  server_ptr = &server;
  HdlcRpcIngress ingress(server, pw::ByteSpan(decoder_storage));
  ingress_ptr = &ingress;
  (void)server_ptr;
  (void)ingress_ptr;

  ProbeService service;
  server.RegisterService(service);

  ClientToServerOutput client_output(server);
  std::array<pw::rpc::Channel, 1> client_channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&client_output)};
  pw::rpc::Client client(pw::span<pw::rpc::Channel>(client_channels.data(),
                                                     client_channels.size()));
  server_output.set_client(client);

  using RawProbeClient = ::avr::port::rpc::pw_rpc::raw::RpcProbe::Client;
  RawProbeClient raw_client(client, kRpcChannelId);

  const std::array<std::byte, 3> request = {
      std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  expected_request = request;
  auto unary_call = raw_client.Unary(request, OnUnaryResponse);
  (void)unary_call;
  if (!service.unary_called() || service.unary_request_size() != request.size() ||
      !unary_response_ok) {
    return 1;
  }

  auto bidi_call = raw_client.Bidi();
  bidi_call.set_on_next(OnBidiResponse);
  if (!bidi_call.Write(request).ok()) return 1;
  if (!service.bidi_server_received() || !bidi_response_ok) return 1;
  (void)bidi_call.Cancel();

  // A lone HDLC flag is an incomplete frame, not a malformed packet.
  return ingress.ProcessByte(std::byte{0x7e}) == pw::Status::Unavailable()
             ? 0
             : 1;
}
