// File: pw_log_avr/public_overrides/pw_log_backend/log_backend.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pw_log_avr/log_avr.h"
#include "pw_tokenizer/tokenize.h"

#define PW_HANDLE_LOG(level, module, flags, message, ...)                      \
  do {                                                                         \
    uint8_t _pw_log_buf[24];                                                   \
    size_t _pw_log_size = sizeof(_pw_log_buf);                                 \
    PW_TOKENIZE_TO_BUFFER(_pw_log_buf, &_pw_log_size, message, ##__VA_ARGS__); \
    pw_log_avr_HandleLog(                                                      \
        static_cast<uint8_t>(level), _pw_log_buf, _pw_log_size);               \
  } while (0)
