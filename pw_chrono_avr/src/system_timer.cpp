// File: pw_chrono_avr/src/system_timer.cpp
#include "pw_chrono_avr/system_timer.h"

#include <utility>

namespace pw::chrono::avr {

void CooperativeSystemTimer::InvokeAfter(uint32_t delay_ms, Callback callback) {
  InvokeAt(pw_chrono_avr_GetMillis() + delay_ms, std::move(callback));
}

void CooperativeSystemTimer::InvokeAt(uint32_t deadline_ms, Callback callback) {
  deadline_ms_ = deadline_ms;
  callback_ = std::move(callback);
  armed_ = static_cast<bool>(callback_);
}

void CooperativeSystemTimer::Cancel() {
  armed_ = false;
  callback_ = nullptr;
}

bool CooperativeSystemTimer::Poll() {
  if (!armed_) {
    return false;
  }
  const uint32_t now_ms = pw_chrono_avr_GetMillis();
  if (static_cast<int32_t>(now_ms - deadline_ms_) < 0) {
    return false;
  }
  Callback cb = std::move(callback_);
  armed_ = false;
  callback_ = nullptr;
  if (cb) {
    cb(now_ms);
    return true;
  }
  return false;
}

}  // namespace pw::chrono::avr
