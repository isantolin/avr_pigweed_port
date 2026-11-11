#include <array>
#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_transfer/client.h"
#include "pw_transfer/transfer.h"

int main() {
  std::array<std::byte, 128> chunk_buffer{};
  std::array<std::byte, 128> encode_buffer{};
  pw::transfer::Thread<1, 1> transfer_thread(
      pw::ByteSpan(chunk_buffer.data(), chunk_buffer.size()),
      pw::ByteSpan(encode_buffer.data(), encode_buffer.size()));
  pw::transfer::TransferService service(transfer_thread, 128);
  return transfer_thread.Poll() ? 1 : 0;
}
