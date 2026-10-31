// File: src/PigweedAVR.h
#pragma once

/// Master header for Pigweed AVR Arduino Library
/// Provides full access to ported Pigweed modules and AVR backends.

#include <stdint.h>
#include <stddef.h>

// 1. Foundation and Error Handling
#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_bytes/span.h"
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
#include "pw_cobs/cobs.h"
#include "pw_ring_buffer/prefixed_entry_ring_buffer.h"

// 3. Hardware Adapters for Arduino
#include "arduino_stream.h"
#include "avr_uart_stream.h"

// 4. Crypto and Tokenization
#include "pw_crypto/sha256.h"
#include "pw_tokenizer/hash.h"
#include "pw_string/string.h"
#include "pw_base64/base64.h"

// 5. Diagnostics, Chrono and RPC
#include "pw_chrono/system_clock.h"
#include "pw_chrono_avr/system_clock_avr.h"
#include "pw_assert_avr/assert_avr.h"
#include "pw_log/log.h"
#include "pw_log_avr/log_avr.h"
#include "pw_rpc_avr/rpc_server.h"
