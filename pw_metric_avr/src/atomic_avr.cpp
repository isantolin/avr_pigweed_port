// File: pw_metric_avr/src/atomic_avr.cpp
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__AVR__) || defined(PW_METRIC_AVR_HOST_TEST)

#if defined(__AVR__)
#include <util/atomic.h>
#else
#define ATOMIC_RESTORESTATE 0
#define ATOMIC_BLOCK(type) for (int _atomic_once = 1; _atomic_once; _atomic_once = 0)
#endif

extern "C" {

#define PW_AVR_DEFINE_ATOMIC_OPS(SIZE, TYPE)                                   \
  TYPE __attribute__((weak))                                                   \
  __atomic_load_##SIZE(const volatile void* ptr, int memorder) {               \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      result = *static_cast<const volatile TYPE*>(ptr);                        \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  void __attribute__((weak))                                                   \
  __atomic_store_##SIZE(volatile void* ptr, TYPE val, int memorder) {          \
    (void)memorder;                                                            \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      *static_cast<volatile TYPE*>(ptr) = val;                                 \
    }                                                                          \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_exchange_##SIZE(volatile void* ptr, TYPE val, int memorder) {       \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = val;                                                                \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_fetch_add_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = static_cast<TYPE>(prev + val);                                      \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_fetch_sub_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = static_cast<TYPE>(prev - val);                                      \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_fetch_and_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = static_cast<TYPE>(prev & val);                                      \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_fetch_or_##SIZE(volatile void* ptr, TYPE val, int memorder) {       \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = static_cast<TYPE>(prev | val);                                      \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_fetch_xor_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE prev = 0;                                                             \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      prev = *p;                                                               \
      *p = static_cast<TYPE>(prev ^ val);                                      \
    }                                                                          \
    return prev;                                                               \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_add_fetch_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      result = static_cast<TYPE>(*p + val);                                    \
      *p = result;                                                             \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_sub_fetch_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      result = static_cast<TYPE>(*p - val);                                    \
      *p = result;                                                             \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_and_fetch_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      result = static_cast<TYPE>(*p & val);                                    \
      *p = result;                                                             \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_or_fetch_##SIZE(volatile void* ptr, TYPE val, int memorder) {       \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      result = static_cast<TYPE>(*p | val);                                    \
      *p = result;                                                             \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  TYPE __attribute__((weak))                                                   \
  __atomic_xor_fetch_##SIZE(volatile void* ptr, TYPE val, int memorder) {      \
    (void)memorder;                                                            \
    TYPE result = 0;                                                           \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      result = static_cast<TYPE>(*p ^ val);                                    \
      *p = result;                                                             \
    }                                                                          \
    return result;                                                             \
  }                                                                            \
                                                                               \
  bool __attribute__((weak)) __atomic_compare_exchange_##SIZE(                 \
      volatile void* ptr, void* expected, TYPE desired, bool weak,             \
      int success_memorder, int failure_memorder) {                            \
    (void)weak;                                                                \
    (void)success_memorder;                                                    \
    (void)failure_memorder;                                                    \
    TYPE exp = 0;                                                              \
    memcpy(&exp, expected, sizeof(TYPE));                                      \
    bool swapped = false;                                                      \
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {                                        \
      volatile TYPE* p = static_cast<volatile TYPE*>(ptr);                     \
      const TYPE cur = *p;                                                     \
      if (cur == exp) {                                                        \
        *p = desired;                                                          \
        swapped = true;                                                        \
      } else {                                                                 \
        exp = cur;                                                             \
      }                                                                        \
    }                                                                          \
    if (!swapped) {                                                            \
      memcpy(expected, &exp, sizeof(TYPE));                                    \
    }                                                                          \
    return swapped;                                                            \
  }

PW_AVR_DEFINE_ATOMIC_OPS(1, uint8_t)
PW_AVR_DEFINE_ATOMIC_OPS(2, uint16_t)
PW_AVR_DEFINE_ATOMIC_OPS(4, uint32_t)

#undef PW_AVR_DEFINE_ATOMIC_OPS

}  // extern "C"

#endif
