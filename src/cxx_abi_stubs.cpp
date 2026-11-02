// File: src/cxx_abi_stubs.cpp
#include <stddef.h>
#include <stdint.h>

void __attribute__((weak)) operator delete(void*) noexcept { __builtin_trap(); }
void __attribute__((weak)) operator delete(void*, size_t) noexcept { __builtin_trap(); }

extern "C" void __attribute__((weak)) __cxa_pure_virtual() { __builtin_trap(); }

#if defined(__AVR__) || defined(ARDUINO_ARCH_AVR)
// Static initialization guards for single-threaded 8-bit architectures
namespace __cxxabiv1 {
__extension__ typedef int __guard __attribute__((mode(__DI__)));

extern "C" int __cxa_guard_acquire(__guard* g) {
  return !*(reinterpret_cast<uint8_t*>(g));
}

extern "C" void __cxa_guard_release(__guard* g) {
  *(reinterpret_cast<uint8_t*>(g)) = 1;
}

extern "C" void __cxa_guard_abort(__guard*) {}
}  // namespace __cxxabiv1
#endif
