#pragma once
#include <cstddef>

namespace pw::transfer {

class TransferThread {
 public:
  void WaitUntilEventIsProcessed() { ++wait_count; }
  size_t wait_count = 0;
};

}  // namespace pw::transfer
