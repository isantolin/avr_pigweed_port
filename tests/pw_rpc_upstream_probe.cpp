#include <array>

#include "avr_uart_stream.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_rpc/server.h"
#include "pw_span/span.h"

namespace {
constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
}  // namespace

int main() {
  // Isolated link probe; production telemetry firmware remains unchanged.
  avr_port::AvrUartStream uart(115200);
  pw::hdlc::RpcChannelOutput output(uart, kHdlcAddress, "avr-upstream-rpc");
  std::array<pw::rpc::Channel, 1> channels = {
      pw::rpc::Channel::Create<kRpcChannelId>(&output)};
  pw::rpc::Server server(pw::span<pw::rpc::Channel>(channels.data(), channels.size()));
  // Force the upstream parser and server dispatch path into the link.
  const pw::Status status = server.ProcessPacket(pw::ConstByteSpan());
  return status.ok() ? 1 : 0;
}
