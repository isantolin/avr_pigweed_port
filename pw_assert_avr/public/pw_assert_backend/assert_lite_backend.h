// File: pw_assert_avr/public/pw_assert_backend/assert_lite_backend.h
#pragma once

#define PW_ASSERT_HANDLE_FAILURE() pw_assert_avr_HandleFailure(__FILE__, __LINE__, nullptr)
