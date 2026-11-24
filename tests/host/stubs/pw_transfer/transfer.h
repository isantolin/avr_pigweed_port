#pragma once
#include <cstddef>
#include <vector>

#include "pw_transfer/handler.h"
#include "pw_transfer/transfer_thread.h"

namespace pw::transfer {

class TransferService {
 public:
  bool RegisterHandler(Handler& handler) {
    ++register_attempts;
    if (fail_next_register) {
      fail_next_register = false;
      return false;
    }
    registered.push_back(&handler);
    return true;
  }

  bool UnregisterHandler(Handler& handler) {
    ++unregister_attempts;
    for (auto it = registered.begin(); it != registered.end(); ++it) {
      if (*it == &handler) {
        registered.erase(it);
        return true;
      }
    }
    return false;
  }

  bool fail_next_register = false;
  size_t register_attempts = 0;
  size_t unregister_attempts = 0;
  std::vector<Handler*> registered;
};

}  // namespace pw::transfer
