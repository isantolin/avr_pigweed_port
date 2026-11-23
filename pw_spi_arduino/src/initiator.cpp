// File: pw_spi_arduino/src/initiator.cpp
#include "pw_spi_arduino/initiator.h"

#if defined(ARDUINO) || __has_include(<SPI.h>)

namespace pw::spi {
namespace {

uint8_t MapSpiMode(ClockPolarity polarity, ClockPhase phase) {
  if (polarity == ClockPolarity::kActiveHigh) {
    return (phase == ClockPhase::kRisingEdge) ? SPI_MODE0 : SPI_MODE1;
  }
  return (phase == ClockPhase::kRisingEdge) ? SPI_MODE2 : SPI_MODE3;
}

}  // namespace

void ArduinoInitiator::Begin() {
  ::SPI.begin();
  initialized_ = true;
}

void ArduinoInitiator::End() {
  ::SPI.end();
  initialized_ = false;
}

Status ArduinoInitiator::DoConfigure(const Config& config) {
  if (config.bits_per_word != BitsPerWord(8)) {
    return Status::InvalidArgument();
  }
  bit_order_ = config.bit_order;
  data_mode_ = MapSpiMode(config.polarity, config.phase);
  return OkStatus();
}

Status ArduinoInitiator::DoWriteRead(ConstByteSpan write_buffer,
                                     ByteSpan read_buffer) {
  if (!initialized_) {
    return Status::FailedPrecondition();
  }
  const size_t total = (write_buffer.size() > read_buffer.size())
                           ? write_buffer.size()
                           : read_buffer.size();
  if (total == 0U) {
    return OkStatus();
  }

  const uint8_t arduino_order =
      (bit_order_ == BitOrder::kLsbFirst) ? LSBFIRST : MSBFIRST;
  const SPISettings settings(max_hz_, arduino_order, data_mode_);

  ::SPI.beginTransaction(settings);
  const uint32_t start_ms = ::millis();
  Status status = OkStatus();
  for (size_t i = 0U; i < total; ++i) {
    if (timeout_ms_ > 0U && (::millis() - start_ms) > timeout_ms_) {
      status = Status::DeadlineExceeded();
      break;
    }
    const uint8_t tx = (i < write_buffer.size())
                           ? static_cast<uint8_t>(write_buffer[i])
                           : 0x00U;
    const uint8_t rx = ::SPI.transfer(tx);
    if (i < read_buffer.size()) {
      read_buffer[i] = static_cast<std::byte>(rx);
    }
  }
  ::SPI.endTransaction();
  return status;
}

Status ArduinoChipSelector::SetActive(bool active) {
  const uint8_t level = (active == active_low_) ? LOW : HIGH;
  ::pinMode(cs_pin_, OUTPUT);
  ::digitalWrite(cs_pin_, level);
  return OkStatus();
}

}  // namespace pw::spi

#endif
