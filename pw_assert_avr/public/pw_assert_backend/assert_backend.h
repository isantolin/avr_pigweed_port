// File: pw_assert_avr/public/pw_assert_backend/assert_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

#define PW_ASSERT_HANDLE_FAILURE(condition_string) \
  pw_assert_avr_HandleFailure(__LINE__)
