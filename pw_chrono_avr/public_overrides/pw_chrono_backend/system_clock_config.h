// File: pw_chrono_avr/public_overrides/pw_chrono_backend/system_clock_config.h
#pragma once

#define PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_NUMERATOR 1
#define PW_CHRONO_SYSTEM_CLOCK_PERIOD_SECONDS_DENOMINATOR 1000

#ifdef __cplusplus

#include "pw_chrono/epoch.h"

namespace pw::chrono::backend {

inline constexpr Epoch kSystemClockEpoch = pw::chrono::Epoch::kTimeSinceBoot;
inline constexpr bool kSystemClockNmiSafe = false;
inline constexpr bool kSystemClockFreeRunning = false;

}  // namespace pw::chrono::backend

#endif  // __cplusplus
