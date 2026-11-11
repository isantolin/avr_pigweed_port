#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <util/delay.h>
#include <avr/sleep.h>

#include "pw_bytes/span.h"
#include "pw_chrono_avr/system_clock_avr.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/server.h"
#include "pw_status/status.h"
#include "pw_stream/memory_stream.h"
#include "pw_transfer/client.h"
#include "pw_transfer/internal/config.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/internal/event.h"
#include "pw_transfer/transfer.h"

namespace {

constexpr uint64_t kHdlcAddress = 1;
constexpr uint32_t kRpcChannelId = 1;
constexpr uint32_t kReadResourceId = 0x1234;
constexpr uint32_t kWriteResourceId = 0x5678;
constexpr uint32_t kCancelResourceId = 0x1235;
constexpr uint32_t kRetryResourceId = 0x1236;
constexpr uint32_t kTimeoutResourceId = 0x1237;
constexpr size_t kHdlcDecoderBufferSize = 256;

bool first_callback_called = false;
bool duplicate_rejected = false;
bool read_complete = false;
bool write_complete = false;
bool reverse_write_complete = false;
bool cancel_complete = false;
bool retry_complete = false;
bool timeout_complete = false;
uint8_t g_pump_failure_reason = 0;
size_t g_client_to_server_frames = 0;
size_t g_server_to_client_frames = 0;
size_t g_server_process_packets = 0;
uint8_t g_server_process_status = 0;
pw::Status read_completion_status = pw::Status::Unknown();
pw::Status write_completion_status = pw::Status::Unknown();
pw::Status reverse_write_completion_status = pw::Status::Unknown();
pw::Status cancel_completion_status = pw::Status::Unknown();
pw::Status retry_completion_status = pw::Status::Unknown();
pw::Status timeout_completion_status = pw::Status::Unknown();

pw::transfer::TransferThread* g_resource_status_thread = nullptr;

void OnDuplicateResourceStatus(
    pw::Status status,
    const pw::transfer::internal::ResourceStatus&) {
  duplicate_rejected = status == pw::Status::Unavailable();
}

void OnFirstResourceStatus(
    pw::Status status,
    const pw::transfer::internal::ResourceStatus&) {
  first_callback_called = status == pw::Status::NotFound();
  // Re-enter while the first callback is still staged. The single-in-flight
  // guard must reject this nested lookup rather than overwrite its callback.
  if (g_resource_status_thread != nullptr) {
    g_resource_status_thread->EnqueueResourceEvent(
        0x999A, OnDuplicateResourceStatus);
  }
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

void OnCancelComplete(pw::Status status) {
  cancel_completion_status = status;
  cancel_complete = true;
}

void OnRetryComplete(pw::Status status) {
  retry_completion_status = status;
  retry_complete = true;
}

void OnTimeoutComplete(pw::Status status) {
  timeout_completion_status = status;
  timeout_complete = true;
}

void MarkFailure(bool& success, uint8_t& failure_code, uint8_t code) {
  success = false;
  if (failure_code == 0) failure_code = code;
}

void EmitResult(bool success, uint8_t failure_code) {
  const uint16_t ubrr =
      static_cast<uint16_t>((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = static_cast<uint8_t>(ubrr >> 8);
  UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR0B = (1 << TXEN0);
  UCSR0A = (1 << TXC0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  char failure_marker[] = "PW_TRANSFER_PROBE_FAIL_00\r\n";
  const size_t code_index = sizeof("PW_TRANSFER_PROBE_FAIL_") - 1;
  failure_marker[code_index] =
      static_cast<char>('0' + (failure_code / 10U));
  failure_marker[code_index + 1] =
      static_cast<char>('0' + (failure_code % 10U));
  const char* marker =
      success ? "PW_TRANSFER_PROBE_PASS\r\n" : failure_marker;
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
  void DropNextFrames(size_t count) { frames_to_drop_ = count; }
  void SetDropAll(bool enabled) { drop_all_frames_ = enabled; }
  size_t dropped_frames() const { return dropped_frames_; }

 private:
  pw::Status DoWrite(pw::ConstByteSpan data) override {
    if (drop_all_frames_ || frames_to_drop_ > 0) {
      if (frames_to_drop_ > 0) --frames_to_drop_;
      ++dropped_frames_;
      return pw::OkStatus();
    }
    for (std::byte byte : data) {
      auto frame = decoder_.Process(byte);
      if (frame.status() == pw::Status::Unavailable()) continue;
      if (!frame.ok()) return frame.status();
      if (frame->address() != kHdlcAddress) continue;
      if (server_ != nullptr) {
        ++g_client_to_server_frames;
        ++g_server_process_packets;
        pw::Status status = server_->ProcessPacket(frame->data());
        g_server_process_status = static_cast<uint8_t>(status.code());
        if (!status.ok()) return status;
      } else if (client_ != nullptr) {
        ++g_server_to_client_frames;
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
  size_t frames_to_drop_ = 0;
  size_t dropped_frames_ = 0;
  bool drop_all_frames_ = false;
  std::array<std::byte, kHdlcDecoderBufferSize> decoder_storage_{};
  pw::hdlc::Decoder decoder_;
};

bool PumpTransfers(pw::transfer::Thread<1, 1>& client_thread,
                   pw::transfer::Thread<1, 1>& server_thread,
                   bool& complete) {
  g_pump_failure_reason = 0;
  g_client_to_server_frames = 0;
  g_server_to_client_frames = 0;
  g_server_process_packets = 0;
  g_server_process_status = 0;
  for (size_t i = 0; i < 2048 && !complete; ++i) {
    const pw::Status client_status = client_thread.Poll(2);
    const pw::Status server_status = server_thread.Poll(2);
    if (client_status == pw::Status::ResourceExhausted()) {
      g_pump_failure_reason = 1;
      return false;
    }
    if (server_status == pw::Status::ResourceExhausted()) {
      g_pump_failure_reason = 2;
      return false;
    }
    if (!client_status.ok() &&
        client_status != pw::Status::Unavailable()) {
      g_pump_failure_reason = 3;
      return false;
    }
    if (!server_status.ok() &&
        server_status != pw::Status::Unavailable()) {
      g_pump_failure_reason = 4;
      return false;
    }
    // Advance the AVR system clock so timeout/retry paths are deterministic
    // under simavr rather than depending on host-side wall-clock timing.
    pw_chrono_avr_StepMillis(1);
  }
  if (!complete) {
    if (g_client_to_server_frames < 2) {
      if (client_thread.PendingEventsForTest() != 0) {
        g_pump_failure_reason = 17;
      } else if (client_thread.ActiveClientTransfersForTest() == 0) {
        g_pump_failure_reason = 18;
      } else {
        g_pump_failure_reason = 19;
      }
    } else if (g_server_to_client_frames == 0) {
      if (g_client_to_server_frames < 2) {
        g_pump_failure_reason = 8;
      } else if (g_server_process_packets < 2) {
        g_pump_failure_reason = 9;
      } else if (g_server_process_status != 0) {
        g_pump_failure_reason = 10;
      } else if (server_thread.PendingEventsForTest() != 0) {
        g_pump_failure_reason = static_cast<uint8_t>(
            20U + server_thread.PendingEventTypeForTest());
      } else if (server_thread.ActiveServerTransfersForTest() == 0) {
        g_pump_failure_reason = 13;
      } else if (client_thread.PendingEventsForTest() != 0) {
        g_pump_failure_reason = 14;
      } else if (client_thread.ActiveClientTransfersForTest() == 0) {
        g_pump_failure_reason = 15;
      } else {
        g_pump_failure_reason = 16;
      }
    } else {
      g_pump_failure_reason = 7;
    }
    return false;
  }

  // Completion is reported by the client before the peer necessarily handles
  // the final acknowledgement. Drain both cooperative endpoints before
  // allowing the next transfer to claim their single active role.
  for (size_t i = 0; i < 2048; ++i) {
    const bool settled =
        client_thread.PendingEventsForTest() == 0 &&
        server_thread.PendingEventsForTest() == 0 &&
        client_thread.ActiveClientTransfersForTest() == 0 &&
        client_thread.ActiveServerTransfersForTest() == 0 &&
        server_thread.ActiveClientTransfersForTest() == 0 &&
        server_thread.ActiveServerTransfersForTest() == 0;
    if (settled) return true;

    const pw::Status client_status = client_thread.Poll(2);
    const pw::Status server_status = server_thread.Poll(2);
    if (client_status == pw::Status::ResourceExhausted()) {
      g_pump_failure_reason = 21;
      return false;
    }
    if (server_status == pw::Status::ResourceExhausted()) {
      g_pump_failure_reason = 22;
      return false;
    }
    if ((!client_status.ok() &&
         client_status != pw::Status::Unavailable()) ||
        (!server_status.ok() &&
         server_status != pw::Status::Unavailable())) {
      g_pump_failure_reason = 23;
      return false;
    }
    pw_chrono_avr_StepMillis(1);
  }
  if (server_thread.ActiveServerTransfersForTest() != 0) {
    g_pump_failure_reason =
        server_thread.TimedOutServerTransfersForTest() != 0 ? 35 : 30;
  } else if (server_thread.PendingEventsForTest() != 0) {
    g_pump_failure_reason = 31;
  } else if (client_thread.ActiveClientTransfersForTest() != 0) {
    g_pump_failure_reason = 32;
  } else if (client_thread.PendingEventsForTest() != 0) {
    g_pump_failure_reason = 33;
  } else {
    g_pump_failure_reason = 34;
  }
  return false;
}

}  // namespace

int main() {
  pw_chrono_avr_InitTimer0();
  sei();

  static std::array<std::byte, 128> client_chunk{};
  static std::array<std::byte, 128> client_encode{};
  static std::array<std::byte, 128> server_chunk{};
  static std::array<std::byte, 128> server_encode{};
  static pw::transfer::Thread<1, 1> client_thread{
      pw::ByteSpan(client_chunk), pw::ByteSpan(client_encode)};
  static pw::transfer::Thread<1, 1> server_thread{
      pw::ByteSpan(server_chunk), pw::ByteSpan(server_encode)};

  // Fill the bounded event queue and verify overflow is explicit, observable,
  // and resets the queue so subsequent transfer events can proceed.
  bool success = true;
  uint8_t failure_code = 0;
  const uint32_t clock_before = pw_chrono_avr_GetMillis();
  pw_chrono_avr_StepMillis(1);
  if (pw_chrono_avr_GetMillis() != clock_before + 1U) {
    MarkFailure(success, failure_code, 32);
  }
  client_thread.EnqueueTestEvent();
  client_thread.EnqueueTestEvent();
  client_thread.EnqueueTestEvent();
  if (client_thread.Poll(0) != pw::Status::ResourceExhausted()) {
    MarkFailure(success, failure_code, 1);
  }

  // The resource-status responder is a single staged upstream callback.
  // Verify the cooperative queue preserves that contract and honors budgets.
  first_callback_called = false;
  duplicate_rejected = false;
  g_resource_status_thread = &client_thread;
  client_thread.EnqueueResourceEvent(0x9999, OnFirstResourceStatus);
  g_resource_status_thread = nullptr;

  if (!duplicate_rejected) MarkFailure(success, failure_code, 31);
  // Resource-status lookup is dispatched inline on the cooperative owner
  // loop; it must not leave a deferred queue event or block waiting for Poll.
  if (!client_thread.Poll(0).ok()) MarkFailure(success, failure_code, 2);
  if (!client_thread.Poll(1).ok()) MarkFailure(success, failure_code, 3);
  if (!first_callback_called) MarkFailure(success, failure_code, 4);

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

  static pw::transfer::TransferService transfer_service(
      server_thread,
      256,
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(100)),
      0);
  server.RegisterService(transfer_service);
  static pw::transfer::Client transfer_client(
      rpc_client, kRpcChannelId, client_thread, 256);

  std::array<std::byte, 32> read_data{};
  for (size_t i = 0; i < read_data.size(); ++i) {
    read_data[i] = static_cast<std::byte>(i + 1);
  }
  pw::stream::MemoryReader read_source{pw::ConstByteSpan(read_data)};
  pw::transfer::ReadOnlyHandler read_handler(kReadResourceId, read_source);
  if (!transfer_service.RegisterHandler(read_handler)) MarkFailure(success, failure_code, 5);

  std::array<std::byte, 64> read_output_buffer{};
  pw::stream::MemoryWriter read_output{pw::ByteSpan(read_output_buffer)};
  read_complete = false;
  g_client_to_server_frames = 0;
  g_server_to_client_frames = 0;
  g_server_process_packets = 0;
  g_server_process_status = 0;
  auto read_result =
      transfer_client.Read(kReadResourceId, read_output, OnReadComplete);
  if (!read_result.ok()) {
    MarkFailure(success, failure_code, 6);
  } else if (!PumpTransfers(client_thread, server_thread, read_complete)) {
    MarkFailure(success, failure_code,
                static_cast<uint8_t>(40U + g_pump_failure_reason));
  }
  if (!read_completion_status.ok() ||
      read_output.bytes_written() != read_data.size() ||
      std::memcmp(read_output.data(), read_data.data(), read_data.size()) != 0) {
    MarkFailure(success, failure_code, 8);
  }

  std::array<std::byte, 32> write_data{};
  for (size_t i = 0; i < write_data.size(); ++i) {
    write_data[i] = static_cast<std::byte>(0xA0 + i);
  }
  std::array<std::byte, 64> write_sink_buffer{};
  pw::stream::MemoryWriter write_sink{pw::ByteSpan(write_sink_buffer)};
  pw::transfer::WriteOnlyHandler write_handler(kWriteResourceId, write_sink);
  if (!transfer_service.RegisterHandler(write_handler)) MarkFailure(success, failure_code, 9);

  pw::stream::MemoryReader write_source{pw::ConstByteSpan(write_data)};
  write_complete = false;
  auto write_result =
      transfer_client.Write(kWriteResourceId, write_source, OnWriteComplete);
  if (!write_result.ok()) {
    MarkFailure(success, failure_code, 10);
  } else if (!PumpTransfers(client_thread, server_thread, write_complete)) {
    MarkFailure(success, failure_code, 11);
  }
  if (!write_completion_status.ok()) {
    MarkFailure(success, failure_code,
                static_cast<uint8_t>(30U +
                    static_cast<uint8_t>(write_completion_status.code())));
  } else if (write_sink.bytes_written() != write_data.size()) {
    MarkFailure(success, failure_code, 23);
  } else if (std::memcmp(write_sink.data(),
                         write_data.data(),
                         write_data.size()) != 0) {
    MarkFailure(success, failure_code, 24);
  }

  // Cancel an in-flight read before its payload is drained. The transfer
  // callback must report CANCELLED and release the client role.
  std::array<std::byte, 256> cancel_data{};
  for (size_t i = 0; i < cancel_data.size(); ++i) {
    cancel_data[i] = static_cast<std::byte>(0x20 + (i & 0x3F));
  }
  pw::stream::MemoryReader cancel_source{pw::ConstByteSpan(cancel_data)};
  pw::transfer::ReadOnlyHandler cancel_handler(kCancelResourceId,
                                                cancel_source);
  if (!transfer_service.RegisterHandler(cancel_handler)) MarkFailure(success, failure_code, 13);

  std::array<std::byte, 256> cancel_output_buffer{};
  pw::stream::MemoryWriter cancel_output{pw::ByteSpan(cancel_output_buffer)};
  cancel_complete = false;
  auto cancel_result =
      transfer_client.Read(kCancelResourceId, cancel_output, OnCancelComplete);
  if (!cancel_result.ok()) {
    MarkFailure(success, failure_code, 14);
  } else {
    (void)client_thread.Poll(1);
    (void)server_thread.Poll(1);
    cancel_result->Cancel();
    if (!PumpTransfers(client_thread, server_thread, cancel_complete)) {
      MarkFailure(success, failure_code, 15);
    }
  }
  if (cancel_completion_status != pw::Status::Cancelled()) MarkFailure(success, failure_code, 16);

  // Drop one server response. A zero initial-chunk timeout forces an upstream
  // retry; the next response is delivered and the transfer must complete.
  std::array<std::byte, 32> retry_data{};
  for (size_t i = 0; i < retry_data.size(); ++i) {
    retry_data[i] = static_cast<std::byte>(0x60 + i);
  }
  pw::stream::MemoryReader retry_source{pw::ConstByteSpan(retry_data)};
  pw::transfer::ReadOnlyHandler retry_handler(kRetryResourceId, retry_source);
  if (!transfer_service.RegisterHandler(retry_handler)) MarkFailure(success, failure_code, 17);
  std::array<std::byte, 64> retry_output_buffer{};
  pw::stream::MemoryWriter retry_output{pw::ByteSpan(retry_output_buffer)};
  server_to_client_writer.DropNextFrames(1);
  if (!transfer_client.set_max_retries(2).ok()) MarkFailure(success, failure_code, 18);
  retry_complete = false;
  auto retry_result = transfer_client.Read(
      kRetryResourceId,
      retry_output,
      OnRetryComplete,
      pw::transfer::cfg::kDefaultClientTimeout,
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(0)));
  if (!retry_result.ok()) {
    MarkFailure(success, failure_code, 19);
  } else if (!PumpTransfers(client_thread, server_thread, retry_complete)) {
    MarkFailure(success, failure_code, 20);
  }
  if (!retry_completion_status.ok() ||
      retry_output.bytes_written() != retry_data.size() ||
      std::memcmp(retry_output.data(), retry_data.data(), retry_data.size()) != 0 ||
      server_to_client_writer.dropped_frames() != 1) {
    MarkFailure(success, failure_code, 21);
  }

  // Drop every server response and exhaust the retry budget. The cooperative
  // Poll path must complete the client with DEADLINE_EXCEEDED.
  std::array<std::byte, 32> timeout_data{};
  pw::stream::MemoryReader timeout_source{pw::ConstByteSpan(timeout_data)};
  pw::transfer::ReadOnlyHandler timeout_handler(kTimeoutResourceId,
                                                 timeout_source);
  if (!transfer_service.RegisterHandler(timeout_handler)) MarkFailure(success, failure_code, 22);
  std::array<std::byte, 64> timeout_output_buffer{};
  pw::stream::MemoryWriter timeout_output{pw::ByteSpan(timeout_output_buffer)};
  server_to_client_writer.SetDropAll(true);
  if (!transfer_client.set_max_retries(1).ok()) MarkFailure(success, failure_code, 23);
  timeout_complete = false;
  auto timeout_result = transfer_client.Read(
      kTimeoutResourceId,
      timeout_output,
      OnTimeoutComplete,
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(100)),
      pw::chrono::SystemClock::for_at_least(std::chrono::milliseconds(0)));
  if (!timeout_result.ok()) {
    MarkFailure(success, failure_code, 24);
  } else if (!PumpTransfers(client_thread, server_thread, timeout_complete)) {
    MarkFailure(success, failure_code,
                static_cast<uint8_t>(50U + g_pump_failure_reason));
  }
  server_to_client_writer.SetDropAll(false);
  if (timeout_completion_status != pw::Status::DeadlineExceeded()) {
    MarkFailure(success, failure_code, 26);
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
    MarkFailure(success, failure_code, 27);
  }
  pw::stream::MemoryReader reverse_source{pw::ConstByteSpan(reverse_data)};
  reverse_write_complete = false;
  auto reverse_result = reverse_transfer_client.Write(
      kWriteResourceId, reverse_source, OnReverseWriteComplete);
  if (!reverse_result.ok()) {
    MarkFailure(success, failure_code, 28);
  } else if (!PumpTransfers(server_thread, client_thread,
                            reverse_write_complete)) {
    MarkFailure(success, failure_code, 29);
  }
  if (!reverse_write_completion_status.ok() ||
      reverse_sink.bytes_written() != reverse_data.size() ||
      std::memcmp(reverse_sink.data(), reverse_data.data(),
                  reverse_data.size()) != 0) {
    MarkFailure(success, failure_code, 30);
  }

  EmitResult(success, failure_code);
  return success ? 0 : 1;
}
