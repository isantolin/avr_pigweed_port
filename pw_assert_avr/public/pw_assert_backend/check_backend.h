// File: pw_assert_avr/public/pw_assert_backend/check_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

// Halts execution with line diagnostics without consuming any SRAM for strings
#define PW_HANDLE_CRASH(...) \
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, nullptr)

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
  pw_assert_avr_HandleFailure(__FILE__, __LINE__, message)
