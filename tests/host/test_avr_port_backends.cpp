#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "Arduino.h"
#include "SPI.h"
#include "pw_analog_avr/analog_input.h"
#include "pw_chrono_avr/system_timer.h"
#include "pw_digital_io_avr/digital_io.h"
#include "pw_spi_avr/initiator.h"
#include "pw_transfer/cooperative_handler_registration.h"

extern "C" uint32_t pw_chrono_avr_GetMillis(void) {
  return host_stub::g_millis;
}

extern "C" {
#define DECLARE_ATOMIC_SHIM(SIZE, TYPE)                                        \
  TYPE __atomic_load_##SIZE(const volatile void*, int);                        \
  void __atomic_store_##SIZE(volatile void*, TYPE, int);                       \
  TYPE __atomic_exchange_##SIZE(volatile void*, TYPE, int);                    \
  TYPE __atomic_fetch_add_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_fetch_sub_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_fetch_and_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_fetch_or_##SIZE(volatile void*, TYPE, int);                    \
  TYPE __atomic_fetch_xor_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_add_fetch_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_sub_fetch_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_and_fetch_##SIZE(volatile void*, TYPE, int);                   \
  TYPE __atomic_or_fetch_##SIZE(volatile void*, TYPE, int);                    \
  TYPE __atomic_xor_fetch_##SIZE(volatile void*, TYPE, int);                   \
  bool __atomic_compare_exchange_##SIZE(                                       \
      volatile void*, void*, TYPE, bool, int, int);

DECLARE_ATOMIC_SHIM(1, uint8_t)
DECLARE_ATOMIC_SHIM(2, uint16_t)
DECLARE_ATOMIC_SHIM(4, uint32_t)
#undef DECLARE_ATOMIC_SHIM
}  // extern "C"

template <typename T>
void VerifyAtomicShimWidth(
    T (*load_fn)(const volatile void*, int),
    void (*store_fn)(volatile void*, T, int),
    T (*exchange_fn)(volatile void*, T, int),
    T (*fetch_add_fn)(volatile void*, T, int),
    T (*fetch_sub_fn)(volatile void*, T, int),
    T (*fetch_and_fn)(volatile void*, T, int),
    T (*fetch_or_fn)(volatile void*, T, int),
    T (*fetch_xor_fn)(volatile void*, T, int),
    T (*add_fetch_fn)(volatile void*, T, int),
    T (*sub_fetch_fn)(volatile void*, T, int),
    T (*and_fetch_fn)(volatile void*, T, int),
    T (*or_fetch_fn)(volatile void*, T, int),
    T (*xor_fetch_fn)(volatile void*, T, int),
    bool (*cas_fn)(volatile void*, void*, T, bool, int, int)) {
  volatile T cell = 0;
  store_fn(&cell, static_cast<T>(10), 0);
  assert(load_fn(&cell, 0) == static_cast<T>(10));
  assert(exchange_fn(&cell, static_cast<T>(25), 0) == static_cast<T>(10));
  assert(load_fn(&cell, 0) == static_cast<T>(25));

  assert(fetch_add_fn(&cell, static_cast<T>(5), 0) == static_cast<T>(25));
  assert(fetch_sub_fn(&cell, static_cast<T>(10), 0) == static_cast<T>(30));
  assert(fetch_or_fn(&cell, static_cast<T>(0x0F), 0) == static_cast<T>(20));
  assert(fetch_and_fn(&cell, static_cast<T>(0x07), 0) == static_cast<T>(0x1F));
  assert(fetch_xor_fn(&cell, static_cast<T>(0x05), 0) == static_cast<T>(0x07));
  assert(load_fn(&cell, 0) == static_cast<T>(0x02));

  assert(add_fetch_fn(&cell, static_cast<T>(8), 0) == static_cast<T>(10));
  assert(sub_fetch_fn(&cell, static_cast<T>(3), 0) == static_cast<T>(7));
  assert(or_fetch_fn(&cell, static_cast<T>(0x10), 0) == static_cast<T>(0x17));
  assert(and_fetch_fn(&cell, static_cast<T>(0x0F), 0) == static_cast<T>(0x07));
  assert(xor_fetch_fn(&cell, static_cast<T>(0x03), 0) == static_cast<T>(0x04));

  T expected = static_cast<T>(0x04);
  assert(cas_fn(&cell, &expected, static_cast<T>(0x42), true, 0, 0));
  assert(load_fn(&cell, 0) == static_cast<T>(0x42));
  assert(expected == static_cast<T>(0x04));

  expected = static_cast<T>(0x01);
  assert(!cas_fn(&cell, &expected, static_cast<T>(0x99), false, 0, 0));
  assert(expected == static_cast<T>(0x42));
  assert(load_fn(&cell, 0) == static_cast<T>(0x42));
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

  // 2. pw_spi_avr: ArduinoInitiator + ChipSelectors
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

  // 3. pw_digital_io_avr: DigitalIn, DigitalOut, DigitalInOut
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

  // 4. pw_analog_avr: ArduinoAnalogInput
  {
    pw::analog::ArduinoAnalogInput ain(0, 1023);
    host_stub::g_analog_val[0] = 512;
    auto limits = ain.GetLimits();
    assert(limits.min == 0 && limits.max == 1023);
    auto sample = static_cast<pw::analog::AnalogInput&>(ain).TryReadUntil(0);
    assert(sample.ok() && sample.value() == 512);
  }

  // 5. pw_metric_avr: 1/2/4-byte __atomic_* shims (host test mode)
  {
    VerifyAtomicShimWidth<uint8_t>(
        __atomic_load_1, __atomic_store_1, __atomic_exchange_1,
        __atomic_fetch_add_1, __atomic_fetch_sub_1, __atomic_fetch_and_1,
        __atomic_fetch_or_1, __atomic_fetch_xor_1, __atomic_add_fetch_1,
        __atomic_sub_fetch_1, __atomic_and_fetch_1, __atomic_or_fetch_1,
        __atomic_xor_fetch_1, __atomic_compare_exchange_1);
    VerifyAtomicShimWidth<uint16_t>(
        __atomic_load_2, __atomic_store_2, __atomic_exchange_2,
        __atomic_fetch_add_2, __atomic_fetch_sub_2, __atomic_fetch_and_2,
        __atomic_fetch_or_2, __atomic_fetch_xor_2, __atomic_add_fetch_2,
        __atomic_sub_fetch_2, __atomic_and_fetch_2, __atomic_or_fetch_2,
        __atomic_xor_fetch_2, __atomic_compare_exchange_2);
    VerifyAtomicShimWidth<uint32_t>(
        __atomic_load_4, __atomic_store_4, __atomic_exchange_4,
        __atomic_fetch_add_4, __atomic_fetch_sub_4, __atomic_fetch_and_4,
        __atomic_fetch_or_4, __atomic_fetch_xor_4, __atomic_add_fetch_4,
        __atomic_sub_fetch_4, __atomic_and_fetch_4, __atomic_or_fetch_4,
        __atomic_xor_fetch_4, __atomic_compare_exchange_4);
  }

  // 6. pw_transfer_avr: CooperativeHandlerRegistration lifecycle & drains
  {
    pw::transfer::Handler handler(0x42U);
    pw::transfer::TransferService service_a;
    pw::transfer::TransferService service_b;
    pw::transfer::TransferThread thread_a;
    pw::transfer::TransferThread thread_b;
    pw::transfer::CooperativeHandlerRegistration reg;

    assert(!reg.IsAttached());
    assert(!reg.IsAttachedTo(service_a));
    assert(!reg.DetachFrom(handler, service_a));
    reg.Detach(handler);  // no-op when detached

    // Failed registration must leave helper detached after draining.
    service_a.fail_next_register = true;
    assert(!reg.Attach(handler, service_a, thread_a));
    assert(!reg.IsAttached());
    assert(service_a.register_attempts == 1U);
    assert(thread_a.wait_count == 1U);

    // Successful attach drains before and after RegisterHandler.
    assert(reg.Attach(handler, service_a, thread_a));
    assert(reg.IsAttached());
    assert(reg.IsAttachedTo(service_a));
    assert(!reg.IsAttachedTo(service_b));
    assert(service_a.registered.size() == 1U);
    assert(thread_a.wait_count == 3U);

    // Re-attaching to the same service is an idempotent no-op.
    assert(reg.Attach(handler, service_a, thread_a));
    assert(service_a.register_attempts == 2U);
    assert(thread_a.wait_count == 3U);

    // DetachFrom with a different service does nothing.
    assert(!reg.DetachFrom(handler, service_b));
    assert(reg.IsAttachedTo(service_a));

    // Re-attaching to service_b detaches from service_a (draining thread_a
    // before & after UnregisterHandler) and attaches to service_b.
    assert(reg.Attach(handler, service_b, thread_b));
    assert(reg.IsAttachedTo(service_b));
    assert(service_a.registered.empty());
    assert(service_a.unregister_attempts == 1U);
    assert(thread_a.wait_count == 5U);
    assert(service_b.registered.size() == 1U);
    assert(thread_b.wait_count == 2U);

    // DetachFrom matching service detaches cleanly and is idempotent.
    assert(reg.DetachFrom(handler, service_b));
    assert(!reg.IsAttached());
    assert(service_b.registered.empty());
    assert(thread_b.wait_count == 4U);
    assert(!reg.DetachFrom(handler, service_b));
  }

  std::cout << "[PASSED] All avr_pigweed_port host backend tests passed!\n";
  return 0;
}
