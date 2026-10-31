// File: pw_assert_avr/src/assert_avr.cpp
#include "pw_assert_avr/assert_avr.h"

#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <stdint.h>

namespace {

pw_assert_avr_PanicHook g_panic_hook = nullptr;

void TxChar(char c) {
#if defined(UDR0)
  while (!(UCSR0A & (1 << UDRE0))) {}
  UDR0 = static_cast<uint8_t>(c);
#elif defined(UDR1)
  while (!(UCSR1A & (1 << UDRE1))) {}
  UDR1 = static_cast<uint8_t>(c);
#else
  (void)c;
#endif
}

void TxString_P(const char* PROGMEM str) {
  if (str == nullptr) return;
  char c;
  while ((c = static_cast<char>(pgm_read_byte(str++))) != '\0') {
    TxChar(c);
  }
}

void TxInt(int value) {
  char buf[8];
  int i = 0;
  if (value < 0) {
    TxChar('-');
    value = -value;
  }
  if (value == 0) {
    TxChar('0');
    return;
  }
  while (value > 0 && i < 7) {
    buf[i++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  while (i > 0) {
    TxChar(buf[--i]);
  }
}

const char kMsgHeader[] PROGMEM = "\r\n!!! [PW_ASSERT FAILED] Line: ";
const char kMsgEnd[] PROGMEM = " !!!\r\n";

}  // namespace

extern "C" {

void pw_assert_avr_SetPanicHook(pw_assert_avr_PanicHook hook) {
  g_panic_hook = hook;
}

void pw_assert_avr_HandleFailure(int line) {
  cli();

  if (g_panic_hook != nullptr) {
    g_panic_hook(line);
  }

  // Diagnostic line report over hardware serial - 0 bytes of SRAM used
  TxString_P(kMsgHeader);
  TxInt(line);
  TxString_P(kMsgEnd);

  while (true) {
#if defined(__GNUC__)
    __builtin_trap();
#endif
  }
}

}  // extern "C"
