// File: pw_log_avr/src/log_avr.cpp
#include "pw_log_avr/log_avr.h"

namespace {
pw_log_avr_LogSink g_log_sink = nullptr;
}

extern "C" {

void pw_log_avr_SetLogSink(pw_log_avr_LogSink sink) {
  g_log_sink = sink;
}

void pw_log_avr_HandleLog(uint8_t level, const uint8_t* log_data, size_t size) {
  if (g_log_sink != nullptr) {
    g_log_sink(level, log_data, size);
  }
}

}  // extern "C"
