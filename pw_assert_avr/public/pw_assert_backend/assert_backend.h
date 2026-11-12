// File: pw_assert_avr/public/pw_assert_backend/assert_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

// The PW_ASSERT facade supplies an expression, not its evaluated value.
// As with CHECK macros, Arduino's framed UART cannot print it; avoid storing
// every assertion expression and source path in scarce SRAM.
#if defined(ARDUINO)
#define PW_ASSERT_HANDLE_FAILURE(expression) \
  pw_assert_avr_HandleFailure(nullptr, __LINE__, nullptr)
#else
#define PW_ASSERT_HANDLE_FAILURE(expression) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, #expression)
#endif
