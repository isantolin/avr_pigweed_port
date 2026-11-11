// File: pw_assert_avr/public/pw_assert_backend/assert_lite_backend.h
#pragma once

#include "pw_assert_avr/assert_avr.h"

#define PW_ASSERT_HANDLE_FAILURE() pw_assert_avr_HandleFailure(__FILE__, __LINE__, 0)
