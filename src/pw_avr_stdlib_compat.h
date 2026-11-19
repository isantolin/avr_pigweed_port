// AVR compatibility for the subset of C++17 allocation facilities required
// by upstream Pigweed when using an AVR allocator with fundamental alignment
// only. The selected avr-libstdcpp already provides std::launder.
#pragma once

#include <new>

#if defined(__cpp_aligned_new)
// Extended-alignment temporary allocations are unsupported by the AVR heap.
// Fail safely rather than returning a potentially misaligned pointer.
inline void* operator new(std::size_t size,
                          std::align_val_t,
                          const std::nothrow_t&) noexcept {
  (void)size;
  return nullptr;
}

inline void operator delete(void* pointer,
                            std::align_val_t,
                            const std::nothrow_t&) noexcept {
  (void)pointer;
}
#endif
