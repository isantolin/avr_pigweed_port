// File: pw_transfer_avr/public/pw_transfer/cooperative_handler_registration.h
#pragma once

#include "pw_transfer/handler.h"
#include "pw_transfer/transfer.h"
#include "pw_transfer/transfer_thread.h"

namespace pw::transfer {

/// Zero-heap, zero-vtable lifecycle helper for registering a pw::transfer
/// Handler with a TransferService on a cooperative TransferThread.
///
/// In cooperative mode (PW_TRANSFER_COOPERATIVE), RegisterHandler() and
/// UnregisterHandler() enqueue events into a bounded ring buffer rather than
/// blocking on a background worker thread. Draining the thread via
/// WaitUntilEventIsProcessed() before and after each mutation prevents queue
/// overflow and guarantees the handler is detached before destruction or
/// re-attachment.
///
/// Using composition instead of a polymorphic mixin avoids emitting
/// intermediate vtables in AVR .data SRAM and shares a single non-template
/// implementation across ReadOnlyHandler, WriteOnlyHandler, and
/// ReadWriteHandler subclasses.
class CooperativeHandlerRegistration final {
 public:
  bool Attach(Handler& handler,
              TransferService& service,
              TransferThread& thread) {
    if (service_ == &service) {
      return true;
    }
    Detach(handler);
    thread.WaitUntilEventIsProcessed();
    if (!service.RegisterHandler(handler)) {
      return false;
    }
    thread.WaitUntilEventIsProcessed();
    service_ = &service;
    thread_ = &thread;
    return true;
  }

  void Detach(Handler& handler) {
    if (service_ == nullptr) {
      return;
    }
    TransferService* const service = service_;
    TransferThread* const thread = thread_;
    service_ = nullptr;
    thread_ = nullptr;
    thread->WaitUntilEventIsProcessed();
    (void)service->UnregisterHandler(handler);
    thread->WaitUntilEventIsProcessed();
  }

  bool DetachFrom(Handler& handler, const TransferService& service) {
    if (service_ != &service) {
      return false;
    }
    Detach(handler);
    return true;
  }

  bool IsAttached() const { return service_ != nullptr; }

  bool IsAttachedTo(const TransferService& service) const {
    return service_ == &service;
  }

 private:
  TransferService* service_ = nullptr;
  TransferThread* thread_ = nullptr;
};

}  // namespace pw::transfer
