// File: pw_crypto_avr/src/sha256_avr.cpp
#include "pw_crypto/sha256.h"

#include <string.h>

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#include <avr/pgmspace.h>
#define READ_K(idx) pgm_read_dword(&kSha256K[idx])
#define SHA256_PROGMEM PROGMEM
#else
#define READ_K(idx) kSha256K[idx]
#define SHA256_PROGMEM
#endif

namespace pw::crypto::sha256::backend {

namespace {

const uint32_t kSha256K[64] SHA256_PROGMEM = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

inline uint32_t RotR(uint32_t x, uint8_t n) {
  return (x >> n) | (x << (32 - n));
}

inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}

inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t Sigma0(uint32_t x) {
  return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22);
}

inline uint32_t Sigma1(uint32_t x) {
  return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25);
}

inline uint32_t Gamma0(uint32_t x) {
  return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3);
}

inline uint32_t Gamma1(uint32_t x) {
  return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10);
}

void Transform(NativeSha256Context& ctx, const uint8_t* chunk) {
  uint32_t w[64];
  for (uint8_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(chunk[i * 4]) << 24) |
           (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8) |
           (static_cast<uint32_t>(chunk[i * 4 + 3]));
  }
  for (uint8_t i = 16; i < 64; ++i) {
    w[i] = Gamma1(w[i - 2]) + w[i - 7] + Gamma0(w[i - 15]) + w[i - 16];
  }

  uint32_t a = ctx.state[0];
  uint32_t b = ctx.state[1];
  uint32_t c = ctx.state[2];
  uint32_t d = ctx.state[3];
  uint32_t e = ctx.state[4];
  uint32_t f = ctx.state[5];
  uint32_t g = ctx.state[6];
  uint32_t h = ctx.state[7];

  for (uint8_t i = 0; i < 64; ++i) {
    const uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + READ_K(i) + w[i];
    const uint32_t t2 = Sigma0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx.state[0] += a;
  ctx.state[1] += b;
  ctx.state[2] += c;
  ctx.state[3] += d;
  ctx.state[4] += e;
  ctx.state[5] += f;
  ctx.state[6] += g;
  ctx.state[7] += h;
}

}  // namespace

Status DoInit(NativeSha256Context& ctx) {
  ctx.state[0] = 0x6a09e667UL;
  ctx.state[1] = 0xbb67ae85UL;
  ctx.state[2] = 0x3c6ef372UL;
  ctx.state[3] = 0xa54ff53aUL;
  ctx.state[4] = 0x510e527fUL;
  ctx.state[5] = 0x9b05688cUL;
  ctx.state[6] = 0x1f83d9abUL;
  ctx.state[7] = 0x5be0cd19UL;
  ctx.total_bits = 0;
  ctx.buffer_len = 0;
  return OkStatus();
}

Status DoUpdate(NativeSha256Context& ctx, ConstByteSpan data) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
  size_t len = data.size();

  ctx.total_bits += static_cast<uint64_t>(len) * 8ULL;

  while (len > 0) {
    const size_t to_copy = (len < (64 - ctx.buffer_len)) ? len : (64 - ctx.buffer_len);
    memcpy(&ctx.buffer[ctx.buffer_len], ptr, to_copy);
    ctx.buffer_len += to_copy;
    ptr += to_copy;
    len -= to_copy;

    if (ctx.buffer_len == 64) {
      Transform(ctx, ctx.buffer);
      ctx.buffer_len = 0;
    }
  }

  return OkStatus();
}

Status DoFinal(NativeSha256Context& ctx, ByteSpan out_digest) {
  if (out_digest.size() < kDigestSizeBytes) {
    return Status::InvalidArgument();
  }

  // Padding
  ctx.buffer[ctx.buffer_len++] = 0x80;
  if (ctx.buffer_len > 56) {
    memset(&ctx.buffer[ctx.buffer_len], 0, 64 - ctx.buffer_len);
    Transform(ctx, ctx.buffer);
    ctx.buffer_len = 0;
  }
  memset(&ctx.buffer[ctx.buffer_len], 0, 56 - ctx.buffer_len);

  // Append total bits (big endian)
  for (uint8_t i = 0; i < 8; ++i) {
    ctx.buffer[63 - i] = static_cast<uint8_t>(ctx.total_bits >> (i * 8));
  }
  Transform(ctx, ctx.buffer);

  // Store output digest (big endian)
  uint8_t* out = reinterpret_cast<uint8_t*>(out_digest.data());
  for (uint8_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<uint8_t>(ctx.state[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
  }

  return OkStatus();
}

}  // namespace pw::crypto::sha256::backend
