// File: pw_assert_avr/public/pw_assert_avr/assert_avr.h
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pw_assert_avr_PanicHook)(int line);

/// Sets an optional custom panic hook.
void pw_assert_avr_SetPanicHook(pw_assert_avr_PanicHook hook);

/// Diagnostic assertion failure handler for 8-bit AVR.
/// Calls the optional panic hook and halts deterministically. Bare-metal builds
/// emit a UART banner; Arduino builds suppress raw UART output by default to
/// avoid corrupting an active framed serial protocol. Define
/// PW_ASSERT_AVR_ENABLE_UART_DIAGNOSTICS only when the UART is dedicated to logs.
void pw_assert_avr_HandleFailure(const char* file, int line, const char* expression) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
