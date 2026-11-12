// File: pw_assert_avr/src/assert_avr.cpp
#include "pw_assert_avr/assert_avr.h"

#include <stdint.h>

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#else
#include <stdio.h>
#include <stdlib.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif
#endif

namespace {

pw_assert_avr_PanicHook g_panic_hook = nullptr;

#if !defined(ARDUINO) || defined(PW_ASSERT_AVR_ENABLE_UART_DIAGNOSTICS)
void TxChar(char c) {
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#if defined(UDR0)
  while (!(UCSR0A & (1 << UDRE0))) {}
  UDR0 = static_cast<uint8_t>(c);
#elif defined(UDR1)
  while (!(UCSR1A & (1 << UDRE1))) {}
  UDR1 = static_cast<uint8_t>(c);
#else
  (void)c;
#endif
#else
  putchar(c);
#endif
}

void TxString_P(const char* str) {
  if (str == nullptr) return;
  char c;
  while ((c = static_cast<char>(pgm_read_byte(str++))) != '\0') {
    TxChar(c);
  }
}

void TxStringRam(const char* str) {
  if (str == nullptr) return;
  while (*str != '\0') {
    TxChar(*str++);
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

const char kMsgHeader[] PROGMEM = "\r\n!!! [PW_ASSERT FAILED] ";
const char kFileSeparator[] PROGMEM = " : ";
const char kExpressionSeparator[] PROGMEM = " : ";
const char kMsgEnd[] PROGMEM = " !!!\r\n";
#endif  // raw UART diagnostics opt-in for Arduino


}  // namespace

extern "C" {

void pw_assert_avr_SetPanicHook(pw_assert_avr_PanicHook hook) {
  g_panic_hook = hook;
}

void pw_assert_avr_HandleFailure(const char* file, int line, const char* expression) {
#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
  cli();
#endif

  if (g_panic_hook != nullptr) {
    g_panic_hook(line);
  }

#if !defined(ARDUINO) || defined(PW_ASSERT_AVR_ENABLE_UART_DIAGNOSTICS)
  // Bare-metal builds may use UART0 for diagnostics. Arduino builds must opt
  // in explicitly because the active HardwareSerial stream may carry HDLC.
  TxString_P(kMsgHeader);
  TxStringRam(file);
  TxString_P(kFileSeparator);
  TxInt(line);
  TxString_P(kExpressionSeparator);
  TxStringRam(expression);
  TxString_P(kMsgEnd);
#endif

  while (true) {
#if defined(__GNUC__)
    __builtin_trap();
#elif defined(_MSC_VER)
    __debugbreak();
#else
    abort();
#endif
  }
}

}  // extern "C"
