#include <array>
#include <cstddef>

#include "pw_bytes/span.h"
#include "pw_transfer/client.h"
#include "pw_transfer/internal/event.h"
#include "pw_transfer/transfer.h"

int main() {
  std::array<std::byte, 128> chunk_buffer{};
  std::array<std::byte, 128> encode_buffer{};
  pw::transfer::Thread<1, 1> transfer_thread(
      pw::ByteSpan(chunk_buffer.data(), chunk_buffer.size()),
      pw::ByteSpan(encode_buffer.data(), encode_buffer.size()));
  pw::transfer::TransferService service(transfer_thread, 128);

  // The resource-status responder is a single staged upstream callback.
  // Verify the cooperative queue preserves that contract and honors budgets.
  bool first_callback_called = false;
  bool duplicate_rejected = false;
  transfer_thread.EnqueueResourceEvent(
      0x1234,
      [&](pw::Status status,
          const pw::transfer::internal::ResourceStatus&) {
        first_callback_called = status == pw::Status::NotFound();
      });
  transfer_thread.EnqueueResourceEvent(
      0x5678,
      [&](pw::Status status,
          const pw::transfer::internal::ResourceStatus&) {
        duplicate_rejected = status == pw::Status::Unavailable();
      });

  if (!duplicate_rejected) return 1;
  if (transfer_thread.Poll(0) != pw::Status::Unavailable()) return 1;
  if (!transfer_thread.Poll(1).ok()) return 1;
  if (!first_callback_called) return 1;
  return transfer_thread.Poll(2).ok() ? 0 : 1;
}
