// File: pw_assert_avr/public/pw_assert_backend/assert_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

// The PW_ASSERT facade supplies an expression, not its evaluated value.
#define PW_ASSERT_HANDLE_FAILURE(expression) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, #expression)
