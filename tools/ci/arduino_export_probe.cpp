#include <array>
#include <cstddef>

#include "PigweedAVR.h"

namespace {

class NullWriter final : public pw::stream::NonSeekableWriter {
 private:
  pw::Status DoWrite(pw::ConstByteSpan) override { return pw::OkStatus(); }
};

}  // namespace

int main() {
  std::array<std::byte, 128> chunk_storage{};
  std::array<std::byte, 128> encode_storage{};
  pw::transfer::Thread<1, 1> transfer_thread{
      pw::ByteSpan(chunk_storage), pw::ByteSpan(encode_storage)};

  NullWriter writer;
  pw::hdlc::RpcChannelOutput output(writer, 1, "arduino-export-probe");
  std::array<pw::rpc::Channel, 1> channels = {
      pw::rpc::Channel::Create<1>(&output)};
  pw::rpc::Server server(
      pw::span<pw::rpc::Channel>(channels.data(), channels.size()));
  pw::rpc::Client client(
      pw::span<pw::rpc::Channel>(channels.data(), channels.size()));

  pw::transfer::TransferService service(transfer_thread, 256);
  server.RegisterService(service);
  pw::transfer::Client transfer_client(client, 1, transfer_thread, 256);
  (void)transfer_client;
  return 0;
}
