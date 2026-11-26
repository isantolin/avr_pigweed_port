#include <cassert>
#include <cstddef>
#include <cstdint>

#include "Arduino.h"
#include "SPI.h"
#include "pw_analog_avr/analog_input.h"
#include "pw_digital_io_avr/digital_io.h"
#include "pw_spi_avr/initiator.h"

namespace {

void TestAnalogInput() {
  pw_avr_bazel_test::analog_value[4] = 731;
  pw::analog::ArduinoAnalogInput input(4U, 1023);

  const auto limits = input.GetLimits();
  assert(limits.min == 0);
  assert(limits.max == 1023);

  const auto sample =
      input.TryReadUntil(pw::chrono::SystemClock::time_point{});
  assert(sample.ok());
  assert(sample.value() == 731);
}

void TestDigitalIo() {
  pw::digital_io::ArduinoDigitalIn input(9U, true);
  assert(input.Enable().ok());
  assert(pw_avr_bazel_test::pin_mode[9] == INPUT_PULLUP);

  pw_avr_bazel_test::pin_value[9] = HIGH;
  const auto state = input.GetState();
  assert(state.ok());
  assert(state.value() == pw::digital_io::State::kActive);

  assert(input.Disable().ok());
  assert(pw_avr_bazel_test::pin_mode[9] == INPUT);

  pw::digital_io::ArduinoDigitalOut output(
      10U, pw::digital_io::State::kActive);
  assert(output.Enable().ok());
  assert(pw_avr_bazel_test::pin_mode[10] == OUTPUT);
  assert(pw_avr_bazel_test::pin_value[10] == HIGH);
  assert(output.SetState(pw::digital_io::State::kInactive).ok());
  assert(pw_avr_bazel_test::pin_value[10] == LOW);

  // A read-only observation must not change the pin's output mode or latch.
  pw_avr_bazel_test::pin_mode[11] = OUTPUT;
  pw_avr_bazel_test::pin_value[11] = HIGH;
  pw::digital_io::ArduinoDigitalIn observation(11U, false, false);
  assert(observation.Enable().ok());
  const auto observed = observation.GetState();
  assert(observed.ok());
  assert(observed.value() == pw::digital_io::State::kActive);
  assert(pw_avr_bazel_test::pin_mode[11] == OUTPUT);
  assert(pw_avr_bazel_test::pin_value[11] == HIGH);
}

void TestSpi() {
  pw::spi::ArduinoInitiator spi(2000000U, 50U);
  const pw::spi::Config config{
      .polarity = pw::spi::ClockPolarity::kActiveLow,
      .phase = pw::spi::ClockPhase::kFallingEdge,
      .bits_per_word = pw::spi::BitsPerWord(8),
      .bit_order = pw::spi::BitOrder::kLsbFirst,
  };

  assert(spi.Configure(config).ok());
  spi.Begin();
  assert(spi.IsInitialized());
  assert(SPI.begun);

  const std::byte tx[] = {std::byte{0x12}, std::byte{0x34}};
  std::byte rx[2]{};
  SPI.rx_feed = {0xA1U, 0xB2U};
  SPI.rx_index = 0U;

  assert(spi.WriteRead(tx, rx).ok());
  assert(SPI.last_settings.clock_hz == 2000000U);
  assert(SPI.last_settings.bit_order == LSBFIRST);
  assert(SPI.last_settings.data_mode == SPI_MODE3);
  assert(SPI.tx_log.size() == 2U);
  assert(SPI.tx_log[0] == 0x12U);
  assert(SPI.tx_log[1] == 0x34U);
  assert(rx[0] == std::byte{0xA1});
  assert(rx[1] == std::byte{0xB2});
  assert(!SPI.in_transaction);

  pw::spi::NoOpChipSelector no_op;
  assert(no_op.SetActive(true).ok());
  pw::spi::ArduinoChipSelector selector(12U, true);
  assert(selector.SetActive(true).ok());
  assert(pw_avr_bazel_test::pin_mode[12] == OUTPUT);
  assert(pw_avr_bazel_test::pin_value[12] == LOW);

  spi.End();
  assert(!spi.IsInitialized());
  assert(!SPI.begun);
}

}  // namespace

int main() {
  TestAnalogInput();
  TestDigitalIo();
  TestSpi();
  return 0;
}
