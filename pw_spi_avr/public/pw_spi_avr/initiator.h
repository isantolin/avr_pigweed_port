// File: pw_spi_avr/public/pw_spi_avr/initiator.h
#pragma once

#include <stddef.h>
#include <stdint.h>

#if __has_include(<SPI.h>)
#include <Arduino.h>
#include <SPI.h>
#endif

#include "pw_bytes/span.h"
#include "pw_spi/chip_selector.h"
#include "pw_spi/initiator.h"
#include "pw_status/status.h"

namespace pw::spi {

/// Arduino SPI backend implementing pw::spi::Initiator without heap allocation.
class ArduinoInitiator final : public Initiator {
 public:
  explicit constexpr ArduinoInitiator(uint32_t max_hz = 4000000U,
                                      uint32_t timeout_ms = 50U)
      : max_hz_(max_hz),
        timeout_ms_(timeout_ms),
        initialized_(false),
        bit_order_(BitOrder::kMsbFirst),
        data_mode_(0U) {}

  void SetFrequency(uint32_t max_hz) { max_hz_ = max_hz; }
  void SetTimeoutMs(uint32_t timeout_ms) { timeout_ms_ = timeout_ms; }

  void Begin();
  void End();
  bool IsInitialized() const { return initialized_; }

 private:
  Status DoConfigure(const Config& config) override;
  Status DoWriteRead(ConstByteSpan write_buffer, ByteSpan read_buffer) override;

  uint32_t max_hz_;
  uint32_t timeout_ms_;
  bool initialized_;
  BitOrder bit_order_;
  uint8_t data_mode_;
};

/// No-op ChipSelector for targets where CS is managed externally (e.g. GPIO RPC).
class NoOpChipSelector final : public ChipSelector {
 public:
  Status SetActive(bool /*active*/) override { return OkStatus(); }
};

/// Digital-pin ChipSelector for Arduino targets.
class ArduinoChipSelector final : public ChipSelector {
 public:
  explicit constexpr ArduinoChipSelector(uint8_t cs_pin,
                                         bool active_low = true)
      : cs_pin_(cs_pin), active_low_(active_low) {}

  Status SetActive(bool active) override;

 private:
  uint8_t cs_pin_;
  bool active_low_;
};

}  // namespace pw::spi
