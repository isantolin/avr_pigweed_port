// File: pw_assert_avr/public/pw_assert_backend/check_backend.h
#pragma once

#include "pw_preprocessor/compiler.h"

// Halts execution deterministically without storing strings in SRAM
#define PW_HANDLE_CRASH(...) __builtin_trap()

#define PW_HANDLE_ASSERT_FAILURE(condition_string, message, ...) \
  __builtin_trap()

#define PW_HANDLE_ASSERT_BINARY_COMPARE_FAILURE(arg_a_str,         \
                                                arg_a_val,         \
                                                comparison_op_str, \
                                                arg_b_str,         \
                                                arg_b_val,         \
                                                type_fmt,          \
                                                message,           \
                                                ...)               \
  __builtin_trap()
