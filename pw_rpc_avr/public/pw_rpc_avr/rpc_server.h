// File: pw_rpc_avr/public/pw_rpc_avr/rpc_server.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pw_bytes/span.h"
#include "pw_containers/vector.h"
#include "pw_rpc_avr/rpc_packet.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::rpc_avr {

/// Base interface for Pigweed RPC services on AVR.
class RpcService {
 public:
  constexpr explicit RpcService(uint32_t service_id) : id_(service_id) {}
  virtual ~RpcService() = default;

  constexpr uint32_t id() const { return id_; }

  /// Dispatches an incoming method call.
  /// Implementations write their reply into response_buffer and set response_size.
  virtual Status HandleMethod(uint32_t method_id,
                              ConstByteSpan request,
                              ByteSpan response_buffer,
                              size_t& response_size) = 0;

 private:
  uint32_t id_;
};

/// Lightweight RPC Server executing on AVR bare-metal or Arduino core.
/// Dispatches incoming Pigweed RPC requests (from HDLC or Serial) and constructs response frames.
template <size_t kMaxServices = 4>
class RpcServer {
 public:
  constexpr RpcServer() = default;

  Status RegisterService(RpcService& service) {
    if (services_.full()) {
      return Status::ResourceExhausted();
    }
    services_.push_back(&service);
    return OkStatus();
  }

  /// Processes an incoming raw RPC packet and generates the matching response packet.
  StatusWithSize ProcessPacket(ConstByteSpan packet_buffer,
                               ByteSpan response_buffer) {
    Result<RpcPacket> decoded = RpcPacket::Decode(packet_buffer);
    if (!decoded.ok()) {
      return StatusWithSize(decoded.status(), 0);
    }

    const RpcPacket& req = decoded.value();
    if (req.type != PacketType::kRequest) {
      return StatusWithSize::InvalidArgument();
    }

    RpcService* target_service = nullptr;
    for (RpcService* s : services_) {
      if (s != nullptr && s->id() == req.service_id) {
        target_service = s;
        break;
      }
    }

    if (target_service == nullptr) {
      // Service not found - build SERVER_ERROR packet
      RpcPacket err_resp;
      err_resp.type = PacketType::kServerError;
      err_resp.channel_id = req.channel_id;
      err_resp.service_id = req.service_id;
      err_resp.method_id = req.method_id;
      err_resp.call_id = req.call_id;
      err_resp.status = static_cast<uint32_t>(Status::NotFound().code());
      return err_resp.Encode(response_buffer);
    }

    // Allocate temporary subspan for method payload response
    // Reserve at least 32 bytes for packet headers
    if (response_buffer.size() < 32) {
      return StatusWithSize::ResourceExhausted();
    }

    ByteSpan method_payload_buf = response_buffer.subspan(16);
    size_t payload_len = 0;
    const Status method_status = target_service->HandleMethod(
        req.method_id, req.payload, method_payload_buf, payload_len);

    RpcPacket resp;
    resp.type = PacketType::kResponse;
    resp.channel_id = req.channel_id;
    resp.service_id = req.service_id;
    resp.method_id = req.method_id;
    resp.call_id = req.call_id;
    resp.status = static_cast<uint32_t>(method_status.code());
    resp.payload = method_payload_buf.first(payload_len);

    return resp.Encode(response_buffer);
  }

 private:
  Vector<RpcService*, kMaxServices> services_;
};

}  // namespace pw::rpc_avr
