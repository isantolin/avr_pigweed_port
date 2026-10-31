// File: pw_log_avr/public/pw_log_avr/log_avr.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pw_log_avr_LogSink)(uint8_t level, const uint8_t* log_data, size_t size);

/// Sets a custom log sink to route tokenized log packets (e.g. over HDLC channel 1 to OpenWrt daemon).
void pw_log_avr_SetLogSink(pw_log_avr_LogSink sink);

/// Default handler invoked by PW_LOG. Delivers tokenized bytes to the active sink.
void pw_log_avr_HandleLog(uint8_t level, const uint8_t* log_data, size_t size);

#ifdef __cplusplus
}
#endif
