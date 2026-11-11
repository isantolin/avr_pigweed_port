# Cooperative `pw_transfer` backend for AVR

## Scope and pinned upstream

This design targets the exact Pigweed submodule revision recorded by the
repository's `main` gitlink: `55873baa1b37eaf7d0a830515a0846e3fc3a79e2`.
The gitlink was verified in the `main` tree (mode `160000`, type `commit`).
Keep this upstream revision pinned; any upstream adaptation must be an
explicit, reviewable patch, not an untracked edit to the submodule.

The initial integration target is a bare-metal AVR main loop. It must not
create an OS thread, call a blocking wait, or require a heap-backed event queue.

## Verified upstream constraints

The following constraints were identified while reviewing the pinned
revision above. Keep source-level changes and tests tied to this revision:

- `pw::transfer::Thread<ClientSlots, ServerSlots>` owns separate arrays of
  `ClientContext` and `ServerContext`. Therefore `Thread<1, 1>` allows one
  client context and one server context at the same time. It does **not**
  enforce one transfer globally.
- `internal::TransferThread::Run()` is an infinite loop. It waits on
  `TimedThreadNotification` (or `BinarySemaphore`) and processes timeout
  scans after each wake. Calling the inherited thread start path on AVR would
  not return to the application loop.
- The event mailbox is a single `next_event_` slot guarded by
  `next_event_ownership_`. The producer path uses semaphore acquisition;
  simply replacing the consumer wait with a non-blocking poll is not enough
  to make the API cooperative.
- `AddTransferHandler()` and `RemoveTransferHandler()` enqueue an event and
  then call `WaitUntilEventIsProcessed()`. These operations are explicitly
  synchronous and cannot be used from the AVR loop unchanged.
- `StartTransfer()`, chunk delivery, stream replacement, and resource-status
  requests all share the event mailbox. A failed mailbox acquisition can
  reject or drop work; it must not be silently treated as successful.
- Transfer deadlines already use `pw::chrono::SystemClock`. The AVR clock
  backend can supply the monotonic tick source, but the cooperative driver
  must check deadlines on each poll rather than sleep until a deadline.

These constraints are from the exact pinned upstream source, not from a
different Pigweed release.

## Required AVR execution model

Use the Pigweed transfer state machines and protocol implementation, but
replace the thread execution and event handoff with a cooperative scheduler:

```text
AVR main loop
  ├── UART/RPC receive: deliver complete transfer chunk
  ├── TransferPoll(): process a bounded amount of queued work
  ├── TransferPoll(): check client/server deadlines
  └── other application work
```

The scheduler must return on every call. No `pw_thread`, `ThreadCore::Start`,
blocking semaphore acquisition, sleep, busy wait, or unbounded event drain is
permitted on this path.

### Global role arbiter

Keep the upstream storage dimensions at one client and one server context,
but add one explicit global role state:

- `kIdle`
- `kClientActive`
- `kServerActive`

A transfer may claim the idle state before its initial event is accepted.
The role remains claimed through completion, cancellation, timeout, or
protocol failure, and is released exactly once after the context is inactive.
Any new client or server transfer while another role is active is rejected
with `UNAVAILABLE`. This prevents the simultaneous client+server operation
that `Thread<1,1>` otherwise permits.

The arbiter must cover every entry path: locally initiated client transfers,
incoming server initial chunks, retries/restarts, and stream replacement. It
must distinguish a retransmission for the already active session from a new
session; retransmissions must not be rejected as a second transfer.

### Bounded event handoff

Do not keep the upstream single-slot semaphore mailbox as the only producer
handoff. Implement a fixed-capacity, statically allocated event queue suitable
for AVR. The queue must:

- have an explicit capacity and documented maximum event size;
- copy incoming chunk bytes into owned fixed storage before the caller's
  buffer can be reused;
- reject overflow synchronously with `UNAVAILABLE` (or the API's documented
  error channel), never silently drop an accepted event;
- preserve event ordering for start, chunk, cancel, stream, and handler events;
- support producer calls from the main loop and UART/RPC callback context;
- use a short AVR-safe critical section only where an ISR can be a producer;
- expose a non-blocking `ProcessPendingEvents(budget)` operation.

The initial capacity should be deliberately small and justified against SRAM.
Do not introduce dynamic allocation or an unbounded `std::queue`.

### Cooperative transfer driver

Refactor the upstream loop into reusable internal operations rather than
copying the transfer protocol:

1. `InitializeCooperative()`: initialize event ownership/state without
   starting a thread.
2. `ProcessPendingEvents(budget)`: process no more than the specified number
   of events and return immediately when the queue is empty.
3. `ProcessTimeouts(now)`: scan the one active context and deliver timeout
   events when due.
4. `Poll(budget)`: run bounded event processing and timeout processing;
   return a status indicating whether more work remains.

The normal host/threaded Pigweed backend should retain its existing
`Run()` behavior. Prefer factoring shared event handling and timeout logic
so the AVR backend reuses the same `Context::HandleEvent`, chunk parsing,
encoding, retry, and handler state machines.

### Synchronous APIs that need cooperative alternatives

The following upstream APIs cannot be called unchanged from the AVR event
loop:

- `AddTransferHandler` / `RemoveTransferHandler`: currently wait for the
  transfer thread to process their event. Provide immediate cooperative
  registration/removal when called on the owning loop, or completion callbacks
  when cross-context ordering is required.
- `EnqueueResourceEvent`: completion must be asynchronous; the current
  event handoff cannot block.
- stream set/close methods: must enqueue or execute safely without waiting.
- start/cancel/chunk APIs: must report queue-full or role-conflict outcomes
  rather than hiding them behind `void` methods.

Do not implement fake semaphore backends whose `acquire()` returns
immediately. That would violate the synchronization contract and can cause
event overwrite, lost chunks, or use-before-initialization.

## Build and test strategy

Keep the AVR-specific adaptation isolated from the existing host simavr runner.
The host runner added in PR #3 is not a firmware test and does not by itself
prove transfer interoperability.

Required tests before calling the port usable:

1. Compile the pinned Pigweed transfer sources for AVR with static buffers and
   no thread backend.
2. Client transmit and receive state-machine tests.
3. Server transmit and receive state-machine tests.
4. Client-active then server-start attempt returns `UNAVAILABLE`.
5. Server-active then client-start attempt returns `UNAVAILABLE`.
6. Duplicate initial chunk/retry for the active session is accepted.
7. Queue-full behavior is explicit and does not corrupt the active session.
8. Chunk ordering, cancellation, reset, timeout, retry exhaustion, and handler
   removal.
9. Verify each `Poll(budget)` call has bounded work and returns.
10. Compile/link audit confirms no `pw_thread` implementation, OS thread,
    blocking semaphore wait, or heap dependency was pulled into the AVR image.

## Implementation sequence

1. Add the cooperative event/driver seam to the pinned Pigweed transfer
   implementation, preserving the standard threaded path.
2. Add the AVR global role arbiter and static event/chunk storage.
3. Add cooperative handler and stream lifecycle APIs.
4. Wire the driver into an opt-in AVR example without changing the existing
   `main.cpp` telemetry example.
5. Add host unit tests for scheduler semantics and protocol state machines,
   then AVR compile and simavr integration tests.
6. Measure Flash/SRAM against the current firmware before considering the
   feature enabled by default.

## Explicit non-goals

- No simultaneous client and server transfer.
- No second client/server slot.
- No RTOS or `pw_thread` emulation.
- No blocking wait hidden behind a fake backend.
- No rewrite of Pigweed's transfer protocol or chunk state machines.
- No claim of end-to-end UART interoperability based solely on compiling the
  host simavr PTY runner.
