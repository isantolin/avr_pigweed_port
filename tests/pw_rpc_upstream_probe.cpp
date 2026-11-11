#include <array>
#include <cstddef>

#include "avr_uart_stream.h"
#include "pw_bytes/span.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_span/span.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
constexpr size_t kHdlcDecoderBufferSize = 128;

// UART RX is deliberately polled by the owner loop. Pigweed's decoder owns
// no storage; the caller supplies this bounded static frame buffer.
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

}  // namespace

int main() {
  // Isolated link probe; production telemetry firmware remains unchanged.
  avr_port::AvrUartStream uart(115200);
  pw::hdlc::RpcChannelOutput output(uart, kHdlcAddress, "avr-upstream-rpc");

  std::array<pw::rpc::Channel, 1> channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&output)};
  pw::rpc::Server server(pw::span<pw::rpc::Channel>(channels.data(),
                                                     channels.size()));

  std::array<std::byte, kHdlcDecoderBufferSize> decoder_storage{};
  HdlcRpcIngress ingress(server, pw::ByteSpan(decoder_storage));

  // Force the upstream RPC parser/server dispatch path into the link.
  const pw::Status packet_status = server.ProcessPacket(pw::ConstByteSpan());
  // A lone HDLC flag is an incomplete frame, not a malformed packet.
  const pw::Status ingress_status = ingress.ProcessByte(std::byte{0x7e});
  return packet_status.ok() || ingress_status != pw::Status::Unavailable()
             ? 1
             : 0;
}
