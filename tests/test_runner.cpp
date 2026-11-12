// File: tests/test_runner.cpp
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/pgmspace.h>
#include <stdint.h>
#include <stddef.h>
#include <cstring>

#include "pw_status/status.h"
#include "pw_status/try.h"
#include "pw_result/result.h"
#include "pw_span/span.h"
#include "pw_bytes/span.h"
#include "pw_bytes/bit.h"
#include "pw_varint/varint.h"
#include "pw_containers/vector.h"
#include "pw_stream/memory_stream.h"
#include "pw_checksum/crc32.h"
#include "pw_checksum/crc16_ccitt.h"
#include "pw_hdlc/encoder.h"
#include "pw_hdlc/decoder.h"
#include "pw_assert/assert.h"
#include "pw_assert/check.h"
#include "pw_crypto/sha256.h"
#include "pw_ring_buffer/prefixed_entry_ring_buffer.h"
#include "pw_function/function.h"
#include "pw_string/string.h"
#include "pw_base64/base64.h"
#include "pw_tokenizer/hash.h"
#include "pw_chrono/system_clock.h"
#include "pw_chrono_avr/system_clock_avr.h"
#include "pw_log/log.h"
#include "pw_log_avr/log_avr.h"

namespace {

namespace uart {

void Init() {
  const uint16_t ubrr = static_cast<uint16_t>((F_CPU / (16UL * 115200UL)) - 1UL);
  UBRR0H = static_cast<uint8_t>(ubrr >> 8);
  UBRR0L = static_cast<uint8_t>(ubrr & 0xFF);
  UCSR0B = (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void Putc(char c) {
  while (!(UCSR0A & (1 << UDRE0))) {}
  UDR0 = c;
}

// Read strings directly from Flash (PROGMEM) to preserve AVR SRAM
void Puts_P(const char* pstr) {
  for (;;) {
    const char c = static_cast<char>(pgm_read_byte(pstr++));
    if (c == '\0') break;
    Putc(c);
  }
}

void PutInt(int32_t val) {
  if (val < 0) {
    Putc('-');
    val = -val;
  }
  char buf[12];
  int idx = 0;
  if (val == 0) {
    buf[idx++] = '0';
  } else {
    while (val > 0) {
      buf[idx++] = static_cast<char>('0' + (val % 10));
      val /= 10;
    }
  }
  while (idx > 0) {
    Putc(buf[--idx]);
  }
}

void FlushAndHalt() {
  while (!(UCSR0A & (1 << TXC0))) {}
  cli();
  sleep_enable();
  sleep_cpu();
}

void Finish(int total, int passed) {
  Puts_P(PSTR("\r\n[==========] Total tests: "));
  PutInt(total);
  Puts_P(PSTR(", Passed: "));
  PutInt(passed);
  Puts_P(PSTR(", Failed: "));
  PutInt(total - passed);
  Puts_P(PSTR("\r\n"));
  if (total == passed) {
    Puts_P(PSTR("[  PASSED  ] 100% OF PORTED PIGWEED MODULES VERIFIED OK!\r\n"));
  } else {
    Puts_P(PSTR("[  FAILED  ] SOME TESTS FAILED!\r\n"));
  }
  FlushAndHalt();
}

}  // namespace uart

int g_total_tests = 0;
int g_passed_tests = 0;

#define RUN_TEST(name) \
  do { \
    ++g_total_tests; \
    uart::Puts_P(PSTR("[ RUN      ] " #name "\r\n")); \
    if (name()) { \
      ++g_passed_tests; \
      uart::Puts_P(PSTR("[       OK ] " #name "\r\n")); \
    } else { \
      uart::Puts_P(PSTR("[  FAILED  ] " #name "\r\n")); \
    } \
  } while (0)

// 1. Test pw_status
bool TestPwStatus() {
  pw::Status ok = pw::OkStatus();
  if (!ok.ok()) return false;

  pw::Status not_found = pw::Status::NotFound();
  if (not_found.ok()) return false;
  if (not_found != pw::Status::NotFound()) return false;

  pw::Status oom = pw::Status::ResourceExhausted();
  if (oom == ok) return false;

  auto try_helper = [](bool fail) -> pw::Status {
    if (fail) return pw::Status::InvalidArgument();
    return pw::OkStatus();
  };

  auto try_caller = [&](bool fail) -> pw::Status {
    PW_TRY(try_helper(fail));
    return pw::OkStatus();
  };

  if (!try_caller(false).ok()) return false;
  if (try_caller(true) != pw::Status::InvalidArgument()) return false;

  return true;
}

// 2. Test pw_result
bool TestPwResult() {
  pw::Result<int> res(42);
  if (!res.ok()) return false;
  if (res.value() != 42) return false;
  if (*res != 42) return false;

  pw::Result<int> err = pw::Status::OutOfRange();
  if (err.ok()) return false;
  if (err.status() != pw::Status::OutOfRange()) return false;

  auto get_val = [](bool fail) -> pw::Result<uint16_t> {
    if (fail) return pw::Status::Unavailable();
    return 1024U;
  };

  auto try_assign_caller = [&](bool fail) -> pw::Status {
    uint16_t val = 0;
    PW_TRY_ASSIGN(val, get_val(fail));
    if (val != 1024U) return pw::Status::Unknown();
    return pw::OkStatus();
  };

  if (!try_assign_caller(false).ok()) return false;
  if (try_assign_caller(true) != pw::Status::Unavailable()) return false;

  return true;
}

// 3. Test pw_span
bool TestPwSpan() {
  const uint8_t raw[] = {10, 20, 30, 40, 50};
  pw::span<const uint8_t> s(raw);

  if (s.size() != 5) return false;
  if (s.empty()) return false;
  if (s[0] != 10 || s[4] != 50) return false;

  auto sub = s.subspan(1, 3);
  if (sub.size() != 3) return false;
  if (sub[0] != 20 || sub[2] != 40) return false;

  return true;
}

// 4. Test pw_bytes
bool TestPwBytes() {
  std::byte b{0x42};
  if (std::to_integer<uint8_t>(b) != 0x42) return false;

  // Bit extraction
  uint8_t bits = pw::bytes::ExtractBits<uint8_t, 3, 0>(0xAB);
  if (bits != 0x0B) return false;

  // Sign extension
  int8_t ext = pw::bytes::SignExtend<4>(0b1111);
  if (ext != -1) return false;

  return true;
}

// 5. Test pw_varint
bool TestPwVarint() {
  std::byte buf[10];

  // Unsigned 0U -> 1 byte (0x00)
  size_t len = pw::varint::Encode(0U, buf);
  if (len != 1 || buf[0] != std::byte{0x00}) return false;

  // Unsigned 127U -> 1 byte (0x7F)
  len = pw::varint::Encode(127U, buf);
  if (len != 1 || buf[0] != std::byte{0x7F}) return false;

  // Unsigned 300U -> 2 bytes (0xAC, 0x02)
  len = pw::varint::Encode(300U, buf);
  if (len != 2 || buf[0] != std::byte{0xAC} || buf[1] != std::byte{0x02}) return false;

  // Round-trip decode of unsigned value
  uint64_t decoded_val = 0;
  size_t read_bytes = pw::varint::Decode(pw::span(buf, len), &decoded_val);
  if (read_bytes != 2 || decoded_val != 300U) return false;

  // ZigZag encoding for signed integers
  if (pw::varint::ZigZagEncode(0) != 0U) return false;
  if (pw::varint::ZigZagEncode(-1) != 1U) return false;
  if (pw::varint::ZigZagEncode(1) != 2U) return false;
  if (pw::varint::ZigZagEncode(-2) != 3U) return false;

  // ZigZag decode
  if (pw::varint::ZigZagDecode(1U) != -1) return false;
  if (pw::varint::ZigZagDecode(2U) != 1) return false;
  if (pw::varint::ZigZagDecode(3U) != -2) return false;

  return true;
}

// 6. Test pw_containers (pw::Vector)
bool TestPwContainersVector() {
  pw::Vector<uint16_t, 4> vec;

  if (!vec.empty()) return false;
  if (vec.size() != 0) return false;
  if (vec.max_size() != 4) return false;

  vec.push_back(100);
  vec.push_back(200);
  vec.push_back(300);

  if (vec.size() != 3) return false;
  if (vec[0] != 100 || vec[1] != 200 || vec[2] != 300) return false;

  vec.push_back(400);
  if (!vec.full()) return false;

  vec.pop_back();
  if (vec.size() != 3 || vec.full()) return false;

  vec.clear();
  if (!vec.empty()) return false;

  return true;
}

// 7. Test pw_stream
bool TestPwStream() {
  std::byte buffer[16];
  pw::stream::MemoryWriter writer(buffer);

  const std::byte payload[] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
  if (!writer.Write(payload).ok()) return false;
  if (writer.bytes_written() != 4) return false;

  pw::stream::MemoryReader reader(writer.WrittenData());
  std::byte read_buf[4];
  if (!reader.Read(read_buf).ok()) return false;

  if (read_buf[0] != std::byte{0xDE} ||
      read_buf[1] != std::byte{0xAD} ||
      read_buf[2] != std::byte{0xBE} ||
      read_buf[3] != std::byte{0xEF}) {
    return false;
  }

  return true;
}

// 8. Test pw_checksum (CRC-32 standard vector)
bool TestPwCrc32() {
  // Standard ASCII vector "123456789" produces IEEE 802.3 CRC-32: 0xCBF43926
  const char* kVector = "123456789";
  const uint32_t crc = pw::checksum::Crc32::Calculate(
      pw::as_bytes(pw::span(kVector, 9)));

  if (crc != 0xCBF43926UL) return false;

  // Incremental calculation
  pw::checksum::Crc32 incremental;
  incremental.Update(pw::as_bytes(pw::span(kVector, 4)));
  incremental.Update(pw::as_bytes(pw::span(kVector + 4, 5)));

  if (incremental.value() != 0xCBF43926UL) return false;

  return true;
}

// 9. Test pw_checksum (CRC-16 CCITT standard vector)
bool TestPwCrc16() {
  // Standard ASCII vector "123456789" produces CCITT CRC-16: 0x29B1
  const uint16_t crc16 = pw::checksum::Crc16Ccitt::Calculate(
      pw::as_bytes(pw::span("123456789", 9)));

  if (crc16 != 0x29B1U) return false;

  return true;
}

// 10. Test pw_hdlc (Encoder + Decoder full round-trip verification)
bool TestPwHdlcRoundtrip() {
  std::byte raw_stream[64];
  pw::stream::MemoryWriter writer(raw_stream);

  const char* kTestData = "PIGWEED_AVR_OK";
  const uint64_t kAddress = 42;

  // Encode UI frame
  pw::Status encode_status = pw::hdlc::WriteUIFrame(
      kAddress,
      pw::as_bytes(pw::span(kTestData, 14)),
      writer);

  if (!encode_status.ok()) return false;
  if (writer.bytes_written() < 18) return false;

  // Frame must start and end with flag byte 0x7E
  if (raw_stream[0] != std::byte{0x7E}) return false;
  if (raw_stream[writer.bytes_written() - 1] != std::byte{0x7E}) return false;

  // Decode frame using pw::hdlc::DecoderBuffer
  pw::hdlc::DecoderBuffer<64> decoder;
  bool frame_found = false;

  for (const std::byte b : writer.WrittenData()) {
    pw::Result<pw::hdlc::Frame> result = decoder.Process(b);
    if (result.ok()) {
      frame_found = true;
      if (result->address() != kAddress) return false;
      if (result->data().size() != 14) return false;
      if (std::memcmp(result->data().data(), kTestData, 14) != 0) return false;
    }
  }

  return frame_found;
}

// 11. Test pw_assert
bool TestPwAssert() {
  PW_ASSERT(1 + 1 == 2);
  PW_CHECK(2 * 2 == 4);
  PW_CHECK_OK(pw::OkStatus());
  PW_CHECK_INT_EQ(10, 10);
  // Compile the fatal path without executing it.
  if (false) {
    PW_CRASH("compile-only fatal backend check");
  }
  return true;
}

// 12. Test C++17 ABI (Polymorphism and static guard runtime)
bool TestCxxAbi() {
  // Test polymorphism via base class reference
  std::byte buf[16];
  pw::stream::MemoryWriter writer(buf);
  pw::stream::Writer& writer_ref = writer;
  const std::byte b{0x55};
  if (!writer_ref.Write(pw::span(&b, 1)).ok()) return false;

  // Test static local initialization guard
  auto counter = []() -> int {
    static int count = 100;
    return ++count;
  };

  if (counter() != 101) return false;
  if (counter() != 102) return false;

  return true;
}

// 13. Test pw_crypto (SHA-256 NIST vectors, including two-block finalization)
bool TestPwCryptoSha256() {
  const char* msg = "abc";
  std::byte digest[pw::crypto::sha256::kDigestSizeBytes];
  auto status = pw::crypto::sha256::Hash(
      pw::as_bytes(pw::span(msg, 3)), pw::span(digest));
  if (!status.ok()) return false;

  const uint8_t abc_expected[32] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
      0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
      0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  if (std::memcmp(digest, abc_expected, sizeof(abc_expected)) != 0) {
    return false;
  }

  // 56 bytes forces SHA-256 padding into a second compression block.
  char boundary_message[56];
  std::memset(boundary_message, 'a', sizeof(boundary_message));
  status = pw::crypto::sha256::Hash(
      pw::as_bytes(pw::span(boundary_message)), pw::span(digest));
  if (!status.ok()) return false;

  const uint8_t boundary_expected[32] = {
      0xb3, 0x54, 0x39, 0xa4, 0xac, 0x6f, 0x09, 0x48,
      0xb6, 0xd6, 0xf9, 0xe3, 0xc6, 0xaf, 0x0f, 0x5f,
      0x59, 0x0c, 0xe2, 0x0f, 0x1b, 0xde, 0x70, 0x90,
      0xef, 0x79, 0x70, 0x68, 0x6e, 0xc6, 0x73, 0x8a};
  return std::memcmp(digest, boundary_expected, sizeof(boundary_expected)) == 0;
}

// 15. Test pw_ring_buffer
bool TestPwRingBuffer() {
  std::byte raw_buffer[64];
  pw::ring_buffer::PrefixedEntryRingBuffer ring;
  if (!ring.SetBuffer(raw_buffer).ok()) return false;

  const std::byte entry1[] = {std::byte{0xAA}, std::byte{0xBB}};
  const std::byte entry2[] = {std::byte{0xCC}, std::byte{0xDD}, std::byte{0xEE}};

  if (!ring.PushBack(pw::span(entry1)).ok()) return false;
  if (!ring.PushBack(pw::span(entry2)).ok()) return false;
  if (ring.EntryCount() != 2) return false;

  std::byte read_buf[8];
  size_t bytes_read = 0;
  if (!ring.PeekFront(pw::span(read_buf), &bytes_read).ok()) return false;
  if (bytes_read != 2 || read_buf[0] != std::byte{0xAA} || read_buf[1] != std::byte{0xBB}) return false;
  if (!ring.PopFront().ok()) return false;

  if (!ring.PeekFront(pw::span(read_buf), &bytes_read).ok()) return false;
  if (bytes_read != 3 || read_buf[0] != std::byte{0xCC}) return false;
  if (!ring.PopFront().ok()) return false;

  if (ring.EntryCount() != 0) return false;
  return true;
}

// 16. Test pw_function (Zero-heap callbacks and delegates)
bool TestPwFunction() {
  pw::InlineFunction<int(int), 16> triple = [](int val) { return val * 3; };
  if (triple(7) != 21) return false;

  int adder = 100;
  pw::InlineFunction<int(int), 16> add_offset = [&adder](int val) { return val + adder; };
  if (add_offset(5) != 105) return false;

  return true;
}

// 17. Test pw_string (Safe bounded inline strings)
bool TestPwString() {
  pw::InlineString<20> str = "Hello";
  str.append(" AVR");
  if (str.size() != 9) return false;
  if (str != "Hello AVR") return false;
  if (str.capacity() != 20) return false;
  return true;
}

// 18. Test pw_base64
bool TestPwBase64() {
  const char* plain = "Pigweed";
  char encoded[16];
  const size_t encoded_len = pw::base64::Encode(
      pw::as_bytes(pw::span(plain, 7)), pw::span(encoded));
  if (encoded_len != 12) {
    uart::Puts_P(PSTR("Base64: encoded length mismatch\\r\\n"));
    return false;
  }
  if (std::memcmp(encoded, "UGlnd2VlZA==", 12) != 0) {
    uart::Puts_P(PSTR("Base64: encoded payload mismatch\\r\\n"));
    return false;
  }

  std::byte decoded[16]{};
  // Check the C entry point separately from the C++ span/string_view wrapper.
  const size_t c_decoded_len = pw_Base64Decode(encoded, encoded_len, decoded);
  if (c_decoded_len != 7 ||
      std::memcmp(decoded, plain, 7) != 0) {
    uart::Puts_P(PSTR("Base64: C decoder mismatch: "));
    for (size_t i = 0; i < c_decoded_len && i < sizeof(decoded); ++i) {
      uart::PutInt(std::to_integer<uint8_t>(decoded[i]));
      uart::Putc(' ');
    }
    uart::Puts_P(PSTR("\\r\\n"));
    return false;
  }

  std::memset(decoded, 0, sizeof(decoded));
  const size_t decoded_len = pw::base64::Decode(
      std::string_view(encoded, encoded_len), pw::span(decoded));
  if (decoded_len != 7) {
    uart::Puts_P(PSTR("Base64: C++ decoded length mismatch\\r\\n"));
    return false;
  }
  if (std::memcmp(decoded, plain, 7) != 0) {
    uart::Puts_P(PSTR("Base64: C++ decoded payload mismatch: "));
    for (size_t i = 0; i < decoded_len; ++i) {
      uart::PutInt(std::to_integer<uint8_t>(decoded[i]));
      uart::Putc(' ');
    }
    uart::Puts_P(PSTR("\\r\\n"));
    return false;
  }

  return true;
}

// 19. Test pw_tokenizer (Compile-time and runtime string hashing)
bool TestPwTokenizer() {
  constexpr uint32_t token = pw::tokenizer::Hash("Hello AVR Pigweed");
  uint32_t rt_token = pw::tokenizer::Hash("Hello AVR Pigweed");
  if (token == 0) return false;
  if (token != rt_token) return false;
  return true;
}

// 20. Test pw_chrono (SystemClock duration and time points)
bool TestPwChrono() {
  auto t1 = pw::chrono::SystemClock::now();
  auto t2 = pw::chrono::SystemClock::now();
  if (t2 < t1) return false;
  if ((t2 - t1) < pw::chrono::SystemClock::duration::zero()) return false;

  // The explicit tick hook must advance the backend clock deterministically.
  const auto before_step = pw::chrono::SystemClock::now();
  pw_chrono_avr_StepMillis(7);
  const auto after_step = pw::chrono::SystemClock::now();
  return (after_step - before_step) >= pw::chrono::SystemClock::duration(7);
}

// 21. Test pw_log (Tokenized logging to sink)
bool TestPwLog() {
  static bool sink_called = false;
  static uint8_t captured_level = 0;
  static size_t captured_size = 0;

  sink_called = false;
  captured_level = 0;
  captured_size = 0;

  pw_log_avr_SetLogSink([](uint8_t level, const uint8_t* log_data, size_t size) {
    (void)log_data;
    sink_called = true;
    captured_level = level;
    captured_size = size;
  });

  PW_LOG_INFO("AVR Log Test: val = %d", 123);

  pw_log_avr_SetLogSink(nullptr);

  if (!sink_called) return false;
  if (captured_size < 4) return false;  // Must contain at least the 4-byte token
  if (captured_level != PW_LOG_LEVEL_INFO) return false;

  return true;
}


}  // namespace

int main() {
  uart::Init();
  uart::Puts_P(PSTR("\r\n========================================\r\n"));
  uart::Puts_P(PSTR("  PIGWEED AVR 8-BIT FULL PORT TEST SUITE  \r\n"));
  uart::Puts_P(PSTR("========================================\r\n"));

  RUN_TEST(TestPwStatus);
  RUN_TEST(TestPwResult);
  RUN_TEST(TestPwSpan);
  RUN_TEST(TestPwBytes);
  RUN_TEST(TestPwVarint);
  RUN_TEST(TestPwContainersVector);
  RUN_TEST(TestPwStream);
  RUN_TEST(TestPwCrc32);
  RUN_TEST(TestPwCrc16);
  RUN_TEST(TestPwHdlcRoundtrip);
  RUN_TEST(TestPwAssert);
  RUN_TEST(TestCxxAbi);
  RUN_TEST(TestPwCryptoSha256);
  RUN_TEST(TestPwRingBuffer);
  RUN_TEST(TestPwFunction);
  RUN_TEST(TestPwString);
  RUN_TEST(TestPwBase64);
  RUN_TEST(TestPwTokenizer);
  RUN_TEST(TestPwChrono);
  RUN_TEST(TestPwLog);

  uart::Finish(g_total_tests, g_passed_tests);
  return 0;
}
