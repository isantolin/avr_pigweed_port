// File: pw_rpc_avr/src/rpc_server.cpp
#include "pw_rpc_avr/rpc_server.h"
#include "pw_varint/varint.h"
#include <cstring>

namespace pw::rpc_avr {

Result<RpcPacket> RpcPacket::Decode(ConstByteSpan buffer) {
  RpcPacket packet;
  size_t offset = 0;

  while (offset < buffer.size()) {
    uint32_t key = 0;
    size_t key_len = varint::Decode(buffer.subspan(offset), &key);
    if (key_len == 0) {
      return Status::DataLoss();
    }
    offset += key_len;

    const uint32_t field_num = key >> 3;
    const uint32_t wire_type = key & 0x07;

    if (field_num == 1 && wire_type == 0) {  // type (varint)
      uint32_t val = 0;
      size_t len = varint::Decode(buffer.subspan(offset), &val);
      if (len == 0) return Status::DataLoss();
      offset += len;
      packet.type = static_cast<PacketType>(val);
    } else if (field_num == 2 && wire_type == 0) {  // channel_id (varint)
      size_t len = varint::Decode(buffer.subspan(offset), &packet.channel_id);
      if (len == 0) return Status::DataLoss();
      offset += len;
    } else if (field_num == 3 && wire_type == 5) {  // service_id (fixed32)
      if (offset + 4 > buffer.size()) return Status::DataLoss();
      packet.service_id = static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset])) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 1])) << 8) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 2])) << 16) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 3])) << 24);
      offset += 4;
    } else if (field_num == 4 && wire_type == 5) {  // method_id (fixed32)
      if (offset + 4 > buffer.size()) return Status::DataLoss();
      packet.method_id = static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset])) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 1])) << 8) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 2])) << 16) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(buffer[offset + 3])) << 24);
      offset += 4;
    } else if (field_num == 5 && wire_type == 2) {  // payload (length-delimited)
      uint32_t payload_len = 0;
      size_t len = varint::Decode(buffer.subspan(offset), &payload_len);
      if (len == 0) return Status::DataLoss();
      offset += len;
      if (offset + payload_len > buffer.size()) return Status::DataLoss();
      packet.payload = buffer.subspan(offset, payload_len);
      offset += payload_len;
    } else if (field_num == 6 && wire_type == 0) {  // status (varint)
      size_t len = varint::Decode(buffer.subspan(offset), &packet.status);
      if (len == 0) return Status::DataLoss();
      offset += len;
    } else if (field_num == 7 && wire_type == 0) {  // call_id (varint)
      size_t len = varint::Decode(buffer.subspan(offset), &packet.call_id);
      if (len == 0) return Status::DataLoss();
      offset += len;
    } else {
      if (wire_type == 0) {
        uint64_t skip_val;
        size_t len = varint::Decode(buffer.subspan(offset), &skip_val);
        if (len == 0) return Status::DataLoss();
        offset += len;
      } else if (wire_type == 2) {
        uint32_t skip_len;
        size_t len = varint::Decode(buffer.subspan(offset), &skip_len);
        if (len == 0) return Status::DataLoss();
        offset += len + skip_len;
      } else if (wire_type == 5) {
        offset += 4;
      } else if (wire_type == 1) {
        offset += 8;
      } else {
        return Status::DataLoss();
      }
    }
  }

  return packet;
}

StatusWithSize RpcPacket::Encode(ByteSpan destination) const {
  size_t written = 0;

  auto write_varint_field = [&](uint32_t field_num, uint32_t value) -> bool {
    const uint32_t key = (field_num << 3) | 0;
    size_t klen = varint::Encode(key, destination.subspan(written));
    if (klen == 0) return false;
    written += klen;

    size_t vlen = varint::Encode(value, destination.subspan(written));
    if (vlen == 0) return false;
    written += vlen;
    return true;
  };

  auto write_fixed32_field = [&](uint32_t field_num, uint32_t value) -> bool {
    const uint32_t key = (field_num << 3) | 5;
    size_t klen = varint::Encode(key, destination.subspan(written));
    if (klen == 0) return false;
    written += klen;

    if (written + 4 > destination.size()) return false;
    destination[written++] = static_cast<std::byte>(value & 0xFF);
    destination[written++] = static_cast<std::byte>((value >> 8) & 0xFF);
    destination[written++] = static_cast<std::byte>((value >> 16) & 0xFF);
    destination[written++] = static_cast<std::byte>((value >> 24) & 0xFF);
    return true;
  };

  if (!write_varint_field(1, static_cast<uint32_t>(type))) return StatusWithSize::ResourceExhausted();
  if (channel_id != 0 && !write_varint_field(2, channel_id)) return StatusWithSize::ResourceExhausted();
  if (!write_fixed32_field(3, service_id)) return StatusWithSize::ResourceExhausted();
  if (!write_fixed32_field(4, method_id)) return StatusWithSize::ResourceExhausted();

  if (!payload.empty()) {
    const uint32_t key = (5 << 3) | 2;
    size_t klen = varint::Encode(key, destination.subspan(written));
    if (klen == 0) return StatusWithSize::ResourceExhausted();
    written += klen;

    size_t plen = varint::Encode(static_cast<uint32_t>(payload.size()), destination.subspan(written));
    if (plen == 0) return StatusWithSize::ResourceExhausted();
    written += plen;

    if (written + payload.size() > destination.size()) return StatusWithSize::ResourceExhausted();
    std::memcpy(destination.data() + written, payload.data(), payload.size());
    written += payload.size();
  }

  if (status != 0 && !write_varint_field(6, status)) return StatusWithSize::ResourceExhausted();
  if (call_id != 0 && !write_varint_field(7, call_id)) return StatusWithSize::ResourceExhausted();

  return StatusWithSize(written);
}

}  // namespace pw::rpc_avr
