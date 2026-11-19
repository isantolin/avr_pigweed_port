// AVR compatibility for the subset of C++17 library facilities required by
// upstream Pigweed headers when using the Arduino AVR core's legacy <new>.
//
// AVR's allocator supports fundamental alignment only. Extended-alignment
// temporary allocations therefore fail safely instead of returning a
// potentially misaligned pointer.
#pragma once

#include <new>

#if __cplusplus >= 201703L
namespace std {
template <typename T>
inline T* launder(T* pointer) noexcept {
  return __builtin_launder(pointer);
}
}  // namespace std
#endif

#if defined(__cpp_aligned_new)
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
