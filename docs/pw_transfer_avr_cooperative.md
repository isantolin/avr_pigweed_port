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


## Source-level review at the pinned revision

The implementation was checked against the exact submodule commit
`55873baa1b37eaf7d0a830515a0846e3fc3a79e2`, using the Pigweed mirror at
the same commit. The following details refine the constraints above.

### Transfer thread and context storage

- `internal::TransferThread` directly derives from
  `pw::thread::ThreadCore`.
- `Thread<ClientSlots, ServerSlots>` owns two independent arrays:
  `std::array<ClientContext, ClientSlots>` and
  `std::array<ServerContext, ServerSlots>`. The two dimensions are not a
  shared global transfer limit.
- `FindNewTransfer()` searches the selected array for an already-active
  matching session (restart), otherwise returns an inactive context. It does
  not arbitrate across client and server arrays.
- `Context::HandleEvent()` contains the protocol state machine. It should
  remain the shared protocol implementation; the cooperative layer should
  change scheduling and event delivery, not duplicate chunk handling,
  retransmission, handshake, or completion logic.

### Event handoff and observable failure behavior

The upstream handoff is exactly one `Event next_event_`, one shared
`chunk_buffer_`, a `BinarySemaphore next_event_ownership_`, and a
`TimedThreadNotification event_notification_`.

- Producers acquire ownership before writing `next_event_`; the consumer
  releases ownership after `HandleEvent()`.
- `ProcessChunk()` copies the incoming bytes into the shared chunk buffer
  only after it acquires ownership. If acquisition times out, the function
  returns without reporting failure to its caller.
- `StartTransfer()`, `SendStatus()`, `EndTransfer()`, and
  `UpdateClientTransfer()` likewise return silently when
  `TryWaitForEventToProcess()` fails. Their public-facing paths therefore
  cannot reliably report queue saturation or handoff failure today.
- `TransferHandlerEvent()` is the exception: it returns false when it
  cannot acquire the event slot. However, `AddTransferHandler()` and
  `RemoveTransferHandler()` then wait for the consumer to process the event.
- Client and server stream changes enqueue `kSetStream` and then wait for
  processing. This is another synchronous dependency that must be replaced
  for the cooperative backend.
- `EnqueueResourceEvent()` reports `UNAVAILABLE` through its callback if
  it cannot acquire the slot; it stores one callback member, so it is not a
  multi-request queue.
- `Event::ChunkEvent` stores a pointer and size, not owned bytes. The shared
  chunk buffer is the backing storage. A replacement queue must own each
  queued chunk's bytes until that event is consumed.

Consequently, a non-blocking implementation cannot be achieved by changing
only `Run()` or by making semaphore acquisition return immediately. Producer
APIs need an explicit acceptance result or asynchronous completion contract,
and the event storage must be redesigned as a bounded queue.

### Execution and timeout behavior

- `Run()` releases the initial event ownership and enters an infinite loop.
  It waits on `event_notification_`, using the earliest context deadline when
  one exists.
- After every wake (including a timeout wake without an event), it scans all
  client contexts and then all server contexts, calling
  `Context::HandleEvent()` for each expired context.
- `GetNextTransferTimeout()` also scans both context arrays. With AVR's
  one-context-per-role configuration, this scan is bounded, but the current
  `Run()` still blocks and never returns to the application loop.
- Timeout processing is already expressed through the shared context state
  machine. A cooperative driver can factor the scan into a bounded
  `ProcessTimeouts(now)` operation without changing the protocol state
  transitions.

### Build dependency evidence

At this pinned revision, the `pw_transfer.core` build target explicitly
depends on `pw_sync:binary_semaphore`, `pw_sync:timed_thread_notification`,
and `pw_thread:thread_core` in Bazel; GN and CMake also declare these
dependencies. Removing the thread backend from the AVR link therefore
requires an explicit cooperative core target or a cleanly factored common
core. It is not enough to omit creation of a `pw::thread::Thread`.

### Design consequences

1. Keep `Context`, `ClientContext`, `ServerContext`, chunk encoding, and
   protocol transitions shared with upstream.
2. Factor the event consumer into reusable bounded operations, with the
   threaded `Run()` retaining its existing wait-and-dispatch behavior.
3. Give the cooperative path its own statically allocated event queue. Chunk
   payloads must be copied into queue-owned storage at enqueue time.
4. Change producer APIs used by AVR so enqueue failure is observable. In
   particular, the current `void` start/chunk/end methods cannot guarantee
   the requested explicit `UNAVAILABLE` behavior without an API adaptation.
5. Keep handler and stream lifecycle changes ordered with transfer events, but
   provide loop-owned immediate operations or asynchronous completion
   callbacks instead of waiting for the thread.
6. Apply the global client/server role arbiter at the point where a new session
   is accepted, while allowing retransmissions/restarts for the already-owned
   session. The current upstream lookup only checks session identity within
   the selected role's context array.
7. Preserve the existing threaded backend and its tests; add cooperative
   tests for bounded polling, event ownership, queue overflow, cross-role
   rejection, duplicate session chunks, and timeout cleanup.

The source review confirms the architecture requirements; it does not yet
constitute an implementation or a successful AVR build.

### Public API call-chain hazards

The pinned source also exposes failure paths above the internal event mailbox:

- `Client::Read()` and `Client::Write()` allocate and return a valid-looking
  handle after calling `StartClientTransfer()`. That internal method returns
  `void`; if event ownership acquisition times out, the transfer is not
  enqueued, but the public call still returns the handle. The cooperative API
  must return a `Result<Handle>` that reflects actual acceptance.
- `TransferService::HandleChunk()` calls `StartServerTransfer()` for an
  initial chunk and then invokes `on_server_transfer_started_`. Since the
  current start method can silently fail to enqueue, the start callback can
  report a start that was never accepted. The cooperative path must invoke
  this callback only after successful context initialization.
- `TransferService::HandleChunk()` also calls `ProcessServerChunk()` for
  non-initial chunks. That path currently has no return status if the event
  handoff fails.
- `Client::OpenReadStream()` and `Client::OpenWriteStream()` call
  `SetClientReadStream()` / `SetClientWriteStream()`. Those methods wait for
  the transfer thread after enqueueing the stream event. Therefore the first
  client Read/Write operation can block before the transfer itself starts.
- Server RPC stream setup similarly calls `SetServerReadStream()` /
  `SetServerWriteStream()`, which wait for event processing.
- `TransferService::GetResourceStatus()` already allows only one outstanding
  responder and protects it with a mutex. The transfer thread separately
  stores only one `resource_status_callback_`. For a single-loop AVR backend,
  preserve the one-request limit but replace the mutex/thread handoff with
  owner-loop sequencing and an asynchronous response.

These call chains mean the AVR adaptation must not be limited to adding a
new public `Poll()` method. The public client and service wrappers must be
adapted so their return values and callbacks accurately reflect whether work
was accepted.

### Reconsidering the event queue for a single-owner AVR loop

The current design specifies a general event queue. The source review suggests
a smaller and safer first implementation is possible if the AVR integration
guarantees that all RPC decoding and `pw_transfer` API calls execute on the
same main-loop owner context, and UART interrupts only place raw bytes into
the UART driver's existing bounded RX buffer:

- Execute transfer events synchronously on the owner loop by calling the
  shared event dispatcher, rather than enqueueing every event into a second
  queue.
- Add a reentrancy guard. A callback invoked while dispatching must not
  recursively mutate the transfer thread; either reject that operation with
  `FAILED_PRECONDITION` or defer it through a small, typed, statically
  allocated deferred-action queue.
- Incoming chunk bytes remain valid for the duration of the synchronous
  dispatcher call, so the single shared chunk buffer can remain the scratch
  buffer. If any chunk can originate from an ISR or be deferred, it must be
  copied into owned queue storage.
- Polling then needs only to process deferred actions (if enabled) and scan
  deadlines. It must have a fixed per-call budget and return.
- This model avoids copying stream wrappers, callbacks, and handler pointers
  into generic queue slots. Those are non-trivial objects with lifetimes and
  ownership semantics; a byte-copy event ring would be unsafe.

This is a conditional simplification, not an assumption: before adopting it,
verify that the AVR RPC transport invokes service/client callbacks synchronously
from the main loop and never from an ISR or another execution context. If that
cannot be guaranteed, use a typed bounded queue with explicit ownership for
each event payload, not a generic memcpy queue.