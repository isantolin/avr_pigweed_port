// File: pw_assert_avr/public_overrides/pw_assert_backend/assert_lite_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

#define PW_ASSERT_LITE_HANDLE_FAILURE(expression) pw_assert_avr_HandleFailure(__LINE__)
