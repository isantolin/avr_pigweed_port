// File: pw_assert_avr/public/pw_assert_avr/assert_avr.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pw_assert_avr_PanicHook)(int line);

/// Sets an optional custom panic hook.
void pw_assert_avr_SetPanicHook(pw_assert_avr_PanicHook hook);

/// Diagnostic assertion failure handler for 8-bit AVR:
/// Receives source line number directly in registers, consuming 0 bytes of SRAM.
/// Emits diagnostic panic banner over hardware UART and halts deterministically.
void pw_assert_avr_HandleFailure(const char* file, int line, const char* expression) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
