// File: pw_crypto_avr/public/pw_crypto/sha256_backend.h
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pw::crypto::sha256::backend {

struct NativeSha256Context {
  uint32_t state[8];
  uint64_t total_bits;
  uint8_t buffer[64];
  size_t buffer_len;
};

}  // namespace pw::crypto::sha256::backend
