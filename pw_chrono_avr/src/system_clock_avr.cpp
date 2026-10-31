// File: pw_chrono_avr/src/system_clock_avr.cpp
#include <stdint.h>

namespace pw::chrono::backend {

namespace {
int64_t g_current_ticks = 0;
}  // namespace

int64_t GetSystemClockTickCount() {
  return ++g_current_ticks;
}

}  // namespace pw::chrono::backend
