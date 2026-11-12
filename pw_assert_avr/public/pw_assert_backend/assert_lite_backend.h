// File: pw_assert_avr/public/pw_assert_backend/assert_lite_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

// Legacy PW_ASSERT_LITE backend entry point.
#define PW_ASSERT_LITE_HANDLE_FAILURE(expression) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, #expression)
