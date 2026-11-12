// File: pw_chrono_avr/src/system_clock_avr.cpp
#include "pw_chrono_avr/system_clock_avr.h"

#include <stdint.h>

#if defined(ARDUINO)
#include <Arduino.h>
#elif defined(__AVR__)
#include <avr/interrupt.h>
#include <avr/io.h>
#endif

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
#include <util/atomic.h>
#else
#ifndef ATOMIC_BLOCK
#define ATOMIC_BLOCK(type) for (int _i = 1; _i--; )
#define ATOMIC_RESTORESTATE
#endif
#endif

namespace {

volatile uint32_t g_system_clock_millis = 0;

}  // namespace

#if !defined(ARDUINO)
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega2560__)
ISR(TIMER0_COMPA_vect) {
  g_system_clock_millis++;
}
#endif
#endif

extern "C" {

void pw_chrono_avr_InitTimer0(void) {
#if defined(ARDUINO)
  // Arduino Core initializes Timer0 at startup for millis() and micros()
#else
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega2560__)
  // Timer0 CTC mode (WGM01 = 1), Prescaler 64 (CS01=1, CS00=1)
  // At 16 MHz: 16000000 / 64 = 250000 ticks/sec -> 250 ticks = 1 ms (OCR0A = 249)
  TCCR0A = (1 << WGM01);
  OCR0A = 249;
  TIMSK0 |= (1 << OCIE0A);
  TCCR0B = (1 << CS01) | (1 << CS00);
#endif
#endif
}

void pw_chrono_avr_StepMillis(uint32_t ms) {
  // In Arduino builds this is an offset applied to millis(); in bare-metal
  // builds it advances the software clock driven by TIMER0_COMPA_vect.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    g_system_clock_millis += ms;
  }
}

uint32_t pw_chrono_avr_GetMillis(void) {
  uint32_t current_ms = 0;
#if defined(ARDUINO)
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    current_ms = static_cast<uint32_t>(millis()) + g_system_clock_millis;
  }
#else
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    current_ms = g_system_clock_millis;
  }
#endif
  return current_ms;
}

}  // extern "C"

namespace pw::chrono::backend {

int64_t GetSystemClockTickCount() {
  return static_cast<int64_t>(pw_chrono_avr_GetMillis());
}

}  // namespace pw::chrono::backend
