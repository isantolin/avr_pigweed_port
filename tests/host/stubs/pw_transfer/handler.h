#pragma once
#include <cstdint>

namespace pw::transfer {

class Handler {
 public:
  explicit constexpr Handler(uint32_t id) : id_(id) {}
  virtual ~Handler() = default;
  constexpr uint32_t id() const { return id_; }

 private:
  uint32_t id_;
};

}  // namespace pw::transfer
