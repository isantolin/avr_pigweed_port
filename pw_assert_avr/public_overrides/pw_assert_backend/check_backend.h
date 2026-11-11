// File: pw_assert_avr/public_overrides/pw_assert_backend/check_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

#define PW_HANDLE_CRASH(msg, ...) pw_assert_avr_HandleFailure(__FILE__, __LINE__, msg)
#define PW_HANDLE_ASSERT_FAILURE(condition, msg, ...) pw_assert_avr_HandleFailure(__FILE__, __LINE__, #condition)
#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(a, op, b, type_fmt, msg, ...) pw_assert_avr_HandleFailure(__FILE__, __LINE__, msg)
