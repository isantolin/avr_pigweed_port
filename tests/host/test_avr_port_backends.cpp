#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "Arduino.h"
#include "SPI.h"
#include "pw_analog_arduino/analog_input.h"
#include "pw_chrono_avr/system_timer.h"
#include "pw_digital_io_arduino/digital_io.h"
#include "pw_spi_arduino/initiator.h"

extern "C" uint32_t pw_chrono_avr_GetMillis(void) {
  return host_stub::g_millis;
}

int main() {
  // 1. CooperativeSystemTimer: normal deadline + 32-bit wraparound + Cancel
  {
    pw::chrono::avr::CooperativeSystemTimer timer;
    bool fired = false;
    uint32_t fired_at = 0;

    host_stub::g_millis = 0xFFFFFFF0U;
    timer.InvokeAfter(32U, [&](uint32_t now) {
      fired = true;
      fired_at = now;
    });
    assert(timer.IsPending());
    assert(!timer.Poll());

    host_stub::g_millis = 0xFFFFFFFEU;
    assert(!timer.Poll());
    assert(!fired);

    host_stub::g_millis = 0x00000010U;  // wrapped past 0xFFFFFFFF
    assert(timer.Poll());
    assert(fired);
    assert(fired_at == 0x00000010U);
    assert(!timer.IsPending());

    fired = false;
    timer.InvokeAfter(10U, [&](uint32_t) { fired = true; });
    assert(timer.IsPending());
    timer.Cancel();
    assert(!timer.IsPending());
    host_stub::g_millis += 20U;
    assert(!timer.Poll());
    assert(!fired);
  }

  // 2. pw_spi_arduino: ArduinoInitiator + ChipSelectors
  {
    pw::spi::ArduinoInitiator spi(2000000U, 50U);
    assert(!spi.IsInitialized());

    const pw::spi::Config cfg{
        .polarity = pw::spi::ClockPolarity::kActiveLow,
        .phase = pw::spi::ClockPhase::kFallingEdge,
        .bits_per_word = pw::spi::BitsPerWord(8),
        .bit_order = pw::spi::BitOrder::kLsbFirst,
    };
    assert(spi.Configure(cfg).ok());

    const pw::spi::Config bad_cfg{
        .polarity = pw::spi::ClockPolarity::kActiveHigh,
        .phase = pw::spi::ClockPhase::kRisingEdge,
        .bits_per_word = pw::spi::BitsPerWord(16),
        .bit_order = pw::spi::BitOrder::kMsbFirst,
    };
    assert(spi.Configure(bad_cfg) == pw::Status::InvalidArgument());

    std::byte tx[3] = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    std::byte rx[3] = {};
    assert(spi.WriteRead(tx, rx) == pw::Status::FailedPrecondition());

    spi.Begin();
    assert(spi.IsInitialized());
    SPI.tx_log.clear();
    SPI.rx_feed = {0xAA, 0xBB, 0xCC};
    SPI.rx_idx = 0;

    assert(spi.WriteRead(tx, rx).ok());
    assert(SPI.last_settings.clock_hz == 2000000U);
    assert(SPI.last_settings.bit_order == LSBFIRST);
    assert(SPI.last_settings.data_mode == SPI_MODE3);
    assert(SPI.tx_log == (std::vector<uint8_t>{0x12, 0x34, 0x56}));
    assert(rx[0] == std::byte{0xAA});
    assert(rx[1] == std::byte{0xBB});
    assert(rx[2] == std::byte{0xCC});

    pw::spi::NoOpChipSelector noop_cs;
    assert(noop_cs.SetActive(true).ok());

    pw::spi::ArduinoChipSelector pin_cs(10, true);
    assert(pin_cs.SetActive(true).ok());
    assert(host_stub::g_pin_mode[10] == OUTPUT);
    assert(host_stub::g_pin_val[10] == LOW);
    assert(pin_cs.SetActive(false).ok());
    assert(host_stub::g_pin_val[10] == HIGH);

    spi.End();
    assert(!spi.IsInitialized());
  }

  // 3. pw_digital_io_arduino: DigitalIn, DigitalOut, DigitalInOut
  {
    pw::digital_io::ArduinoDigitalIn din(4, true);
    assert(din.GetState().status() == pw::Status::FailedPrecondition());
    assert(din.Enable().ok());
    assert(host_stub::g_pin_mode[4] == INPUT_PULLUP);
    host_stub::g_pin_val[4] = HIGH;
    auto s = din.GetState();
    assert(s.ok() && s.value() == pw::digital_io::State::kActive);
    assert(din.Disable().ok());
    assert(host_stub::g_pin_mode[4] == INPUT);

    pw::digital_io::ArduinoDigitalOut dout(5, pw::digital_io::State::kActive);
    assert(dout.SetState(pw::digital_io::State::kInactive) ==
           pw::Status::FailedPrecondition());
    assert(dout.Enable().ok());
    assert(host_stub::g_pin_mode[5] == OUTPUT);
    assert(host_stub::g_pin_val[5] == HIGH);
    assert(dout.SetState(pw::digital_io::State::kInactive).ok());
    assert(host_stub::g_pin_val[5] == LOW);
    assert(dout.Disable().ok());

    pw::digital_io::ArduinoDigitalInOut dio(6, false,
                                            pw::digital_io::State::kInactive);
    assert(dio.Enable().ok());
    assert(dio.SetState(pw::digital_io::State::kActive).ok());
    auto dio_state = dio.GetState();
    assert(dio_state.ok() &&
           dio_state.value() == pw::digital_io::State::kActive);
  }

  // 4. pw_analog_arduino: ArduinoAnalogInput
  {
    pw::analog::ArduinoAnalogInput ain(0, 1023);
    host_stub::g_analog_val[0] = 512;
    auto limits = ain.GetLimits();
    assert(limits.min == 0 && limits.max == 1023);
    auto sample = static_cast<pw::analog::AnalogInput&>(ain).TryReadUntil(0);
    assert(sample.ok() && sample.value() == 512);
  }

  std::cout << "[PASSED] All avr_pigweed_port host backend tests passed!\n";
  return 0;
}
