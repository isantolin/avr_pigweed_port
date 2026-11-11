// File: src/PigweedAVR.h
#pragma once

#ifndef PW_TRANSFER_COOPERATIVE
#define PW_TRANSFER_COOPERATIVE 1
#endif
#ifndef PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY
#define PW_TRANSFER_COOPERATIVE_QUEUE_CAPACITY 2
#endif
#ifndef PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE
#define PW_TRANSFER_COOPERATIVE_CHUNK_BUFFER_SIZE 128
#endif

/// Master header for Pigweed AVR Arduino Library
/// Provides full access to ported Pigweed modules and AVR backends.

#include <stdint.h>
#include <stddef.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef abs
#undef abs
#endif
#ifdef round
#undef round
#endif

#include <type_traits>

#if !defined(__clang__) && defined(__GNUC__) && (__GNUC__ < 8)
#ifndef _PW_HAS_UNIQUE_OBJECT_REPRESENTATIONS_V_DEFINED
#define _PW_HAS_UNIQUE_OBJECT_REPRESENTATIONS_V_DEFINED
namespace std {
template <typename T>
constexpr bool has_unique_object_representations_v = is_trivially_copyable<T>::value;
} // namespace std
#endif
#endif

// 1. Foundation and Error Handling
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_bytes/span.h"

namespace pw {
using byte = std::byte;
}
#include "pw_varint/varint.h"
#include "pw_containers/vector.h"
#include "pw_function/function.h"

// 2. Framing and Data Streaming
#include "pw_stream/stream.h"
#include "pw_stream/memory_stream.h"
#include "pw_checksum/crc32.h"
#include "pw_checksum/crc16_ccitt.h"
#include "pw_hdlc/encoder.h"
#include "pw_hdlc/decoder.h"
#include "pw_hdlc/rpc_channel.h"
#include "pw_rpc/client.h"
#include "pw_rpc/server.h"
#include "pw_rpc/raw/client_reader_writer.h"
#include "pw_rpc/raw/server_reader_writer.h"
#include "pw_transfer/handler.h"
#include "pw_transfer/transfer_thread.h"
#include "pw_transfer/transfer.h"
#include "pw_transfer/client.h"
#include "arduino_stream.h"

// 3. Tokenization and Utilities
#include "pw_tokenizer/hash.h"
#include "pw_string/string.h"
#include "pw_base64/base64.h"

// 4. Diagnostics and Logging
#include "pw_chrono/system_clock.h"
#include "pw_chrono_avr/system_clock_avr.h"
#include "pw_log/log.h"
#include "pw_log_avr/log_avr.h"
