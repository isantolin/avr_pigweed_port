#include <Arduino.h>
#include <PigweedAVR.h>

#include <array>
#include <cstddef>

namespace {

class NullWriter final : public pw::stream::NonSeekableWriter {
 private:
  pw::Status DoWrite(pw::ConstByteSpan) override { return pw::OkStatus(); }
};

std::array<std::byte, 128> chunk_storage{};
std::array<std::byte, 128> encode_storage{};
pw::transfer::Thread<1, 1> transfer_thread{
    pw::ByteSpan(chunk_storage), pw::ByteSpan(encode_storage)};

NullWriter writer;
pw::hdlc::RpcChannelOutput output(writer, 1, "mega-transfer-export-probe");
std::array<pw::rpc::Channel, 1> channels = {
    pw::rpc::Channel::Create<1>(&output)};
pw::rpc::Server server(
    pw::span<pw::rpc::Channel>(channels.data(), channels.size()));
pw::rpc::Client client(
    pw::span<pw::rpc::Channel>(channels.data(), channels.size()));

pw::transfer::TransferService transfer_service(transfer_thread, 256);
pw::transfer::Client transfer_client(client, 1, transfer_thread, 256);

}  // namespace

void setup() {
  server.RegisterService(transfer_service);
}

void loop() {
  // Link-time probe only: the runtime Bridge owns the UART/HDLC pump.
  (void)transfer_thread.Poll(2);
  (void)transfer_client;
}
