// File: pw_chrono_avr/public/pw_chrono_avr/system_clock_avr.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initializes AVR Timer0 for 1 ms periodic interrupts (CTC mode, prescaler 64 at 16 MHz).
/// If compiling under the Arduino framework, this is a no-op as Arduino already configures Timer0.
void pw_chrono_avr_InitTimer0(void);

/// Advances the internal millisecond counter manually (useful for simulated tests or deterministic time steps).
void pw_chrono_avr_StepMillis(uint32_t ms);

/// Returns current milliseconds count with strict atomic protection against word-tearing on 8-bit AVR.
uint32_t pw_chrono_avr_GetMillis(void);

#ifdef __cplusplus
}
#endif
