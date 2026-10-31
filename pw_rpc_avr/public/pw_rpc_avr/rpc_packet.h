// File: pw_rpc_avr/public/pw_rpc_avr/rpc_packet.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pw_bytes/span.h"
#include "pw_result/result.h"
#include "pw_status/status.h"
#include "pw_status/status_with_size.h"

namespace pw::rpc_avr {

enum class PacketType : uint8_t {
  kRequest = 0,
  kResponse = 1,
  kClientStream = 2,
  kClientError = 4,
  kServerError = 5,
  kServerStream = 7,
  kClientRequestCompletion = 8,
};

/// Wire-compatible Pigweed RPC Packet for 8-bit AVR microcontrollers.
/// Implements deterministic encode and decode without heap allocation or heavyweight protobuf compiler dependencies.
struct RpcPacket {
  PacketType type{PacketType::kRequest};
  uint32_t channel_id{0};
  uint32_t service_id{0};
  uint32_t method_id{0};
  ConstByteSpan payload{};
  uint32_t status{0};
  uint32_t call_id{0};

  static Result<RpcPacket> Decode(ConstByteSpan buffer);
  StatusWithSize Encode(ByteSpan destination) const;
};

}  // namespace pw::rpc_avr
