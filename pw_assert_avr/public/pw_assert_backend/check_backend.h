// File: pw_assert_avr/public/pw_assert_backend/check_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

// The AVR backend reports the expression and halts; it does not format varargs.
#define PW_HANDLE_CRASH(message, ...) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, message)

#define PW_HANDLE_ASSERT_FAILURE(condition_string, message, ...) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, condition_string)

#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(arg_a_str,         \
                                                arg_a_val,         \
                                                comparison_op_str, \
                                                arg_b_str,         \
                                                arg_b_val,         \
                                                type_fmt,          \
                                                message,           \
                                                ...)               \
  pw_assert_avr_HandleFailure(                                     \
      __FILE__, __LINE__, arg_a_str " " comparison_op_str " " arg_b_str)
