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
## Integration prerequisite: AVR RPC is not yet a `pw_transfer` transport

Review of this repository's `CMakeLists.txt`, `pw_rpc_avr` public headers,
and `pw_rpc_avr/src/rpc_server.cpp` shows that the existing RPC component is
a lightweight custom dispatcher, not Pigweed's upstream `pw_rpc` server/client
API required by `pw_transfer`.

Important distinction:

- The AVR `RpcPacket` fields (type, channel, service, method, payload, status,
  call ID) match the field numbers and wire types in the pinned upstream
  `pw_rpc/internal/packet.proto`. The packet envelope is therefore compatible
  at the schema level for the fields it implements.
- `pw::rpc_avr::RpcServer::ProcessPacket()` currently accepts only
  `PacketType::kRequest`; other packet types are rejected. It dispatches to
  `RpcService::HandleMethod()` and produces one synchronous response.
- The AVR service interface has no `RawServerReaderWriter`, streaming call
  object, per-call lifecycle, `set_on_next`, `Write`, `Finish`, or matching
  client-side `RawClientReaderWriter` implementation.
- The `pw_transfer` core target at the pinned Pigweed revision includes
  `pw_rpc::raw` client/server reader-writer APIs and requires bidirectional
  streaming. Its service methods are `Read(stream Chunk)` and
  `Write(stream Chunk)`, not unary methods.
- The current AVR `CMakeLists.txt` builds `pw_rpc_avr` but does not add
  upstream `pw_rpc` or `pw_transfer` targets.

Thus the packet format alone is not enough: the current AVR RPC component
cannot host or call the pinned `pw_transfer` implementation. This is a
separate prerequisite from the cooperative transfer scheduler.

### Required order of work

1. Specify and test the minimum streaming RPC subset needed by `pw_transfer`:
   bidirectional calls, request/client-stream/server-stream packet handling,
   call IDs, per-call state, completion, cancellation/error, and bounded
   message storage.
2. Decide whether to adapt the custom `pw_rpc_avr` to the upstream raw RPC
   interfaces or replace it with a minimal upstream `pw_rpc` configuration.
   Do not claim compatibility until the upstream API contract and wire
   interoperability tests pass.
3. Add a host test that exchanges actual Pigweed-generated `pw_rpc` packets
   with the AVR implementation, including a multi-message bidirectional call.
4. Only after that transport prerequisite is satisfied, integrate the
   cooperative `pw_transfer` core and its one-role arbiter.

Do not implement a fake `RawServerReaderWriter` facade over the current unary
dispatcher: transfer depends on persistent bidirectional call state and stream
callbacks, which a one-request/one-response wrapper cannot safely emulate.

### Architecture decision: prefer upstream `pw_rpc`, not a second RPC stack

A further inspection of the repository's current `main` build graph and public
headers changes the migration risk assessment:

- `firmware.elf` does not link `pw_rpc_avr`; only `test_runner.elf` links
  that target. The custom dispatcher is therefore not currently a firmware
  runtime dependency.
- `src/PigweedAVR.h` does not expose `pw_rpc_avr` either.
- The custom implementation is a unary-only test component. Extending it into
  a second RPC stack would duplicate upstream call dispatch, channel, stream,
  and lifecycle machinery and create a long-term compatibility burden.

**Decision:** the preferred path is to compile and use the pinned upstream
`pw_rpc` implementation, with AVR-specific transport and platform backends.
Do not expand `pw_rpc_avr` to implement streaming unless a measured build
constraint proves that upstream `pw_rpc` cannot fit or cannot be configured
for this target.

This is a migration direction, not a claim that upstream `pw_rpc` already
builds on AVR. The following proof is required before deleting the custom
component:

1. Inspect the pinned `pw_rpc` CMake target and its transitive dependencies.
2. Add the minimum upstream `pw_rpc` targets to an isolated AVR integration
   build, without changing the existing firmware target.
3. Implement only the missing AVR platform pieces, beginning with
   `ChannelOutput` and the UART/HDLC packet ingress path.
4. Compile and link a minimal upstream unary service on the actual AVR
   toolchain; record Flash and SRAM use.
5. Exercise a bidirectional raw RPC stream using upstream-generated packets
   on the host and simavr.
6. Add the pinned `pw_transfer` client/server service only after the streaming
   test passes.
7. Remove `pw_rpc_avr` and its tests once no target depends on it and the
   upstream interoperability tests cover the required behavior.

Keep the existing custom component isolated as a temporary compatibility
experiment. Do not add it to `firmware.elf`, and do not make it a prerequisite
for the cooperative `pw_transfer` implementation.


### Build-graph audit on repository main

The current repository CMake file was inspected at base commit
`1b79d287e389955bd8ed9307e4555bab755322d6`:

- It adds `pw_hdlc` and links `pw_hdlc.encoder` into the firmware.
- It does not add the upstream `pw_rpc` directory or link any
  `pw_rpc.server`, `pw_rpc.common`, or raw RPC target.
- It declares a local `pw_rpc_avr` static library from
  `pw_rpc_avr/src/rpc_server.cpp`.
- Only `test_runner.elf` links `pw_rpc_avr`; `firmware.elf` does not.
- `src/PigweedAVR.h` does not include the custom RPC headers.

This confirms the custom dispatcher is not on the production firmware link
path. The migration can be isolated and must not be coupled to the existing
telemetry firmware until the upstream proof passes.

### Upstream RPC integration proof: current status

The upstream Pigweed CMake RPC module exposes distinct core targets for
common packet/channel handling, server dispatch, and client calls; raw RPC
stream APIs are layered on those core targets. The normal RPC test helpers,
fake channel output, synchronous client API, and threaded test helpers bring
additional dependencies and must not be pulled into the AVR image by accident.

The integration must therefore start with the smallest server-only target
closure, then add raw server streaming, and add client targets only if the AVR
device itself must initiate transfers. The UART transport is a separate
`ChannelOutput` implementation plus a packet ingress loop feeding
`Server::ProcessPacket()`; HDLC framing alone is not an RPC endpoint.

**Unverified gate:** the pinned submodule source at
`55873baa1b37eaf7d0a830515a0846e3fc3a79e2` has not yet been successfully
configured and compiled in this environment. Do not copy target names or
transitive dependencies from a different Pigweed revision into the build.
The exact pinned `pw_rpc/CMakeLists.txt`, `pw_rpc/raw/CMakeLists.txt`,
generated-proto configuration, lock backend, and no-heap configuration must
be inspected before changing the root CMake graph.

No source implementation is claimed by this audit. In particular, no
`ChannelOutput`, RPC server, or streaming service has been added, and no
AVR memory figures or interoperability results are available.

### Exit criteria for the implementation PR

The upstream RPC migration is complete only when all of these are evidenced
by CI artifacts/logs:

1. Configure and build the exact pinned `pw_rpc` server target closure using
   the AVR toolchain, with the production telemetry firmware unchanged.
2. Link a minimal server-only image and report `avr-size` Flash/data/BSS.
3. Run host unit tests for UART packet ingress, bounded TX behavior, and
   malformed packet rejection.
4. Exchange a unary request/response with an upstream-generated Pigweed
   packet.
5. Exchange a multi-message bidirectional raw RPC call with correct call IDs,
   final status, cancellation, and stream lifetime.
6. Verify the AVR build has no unresolved mutex/thread backend and no
   accidental dynamic allocation dependency.
7. Only then add `pw_transfer` and its cooperative execution layer.

Until these gates pass, retain `pw_rpc_avr` only as a test-only legacy target;
do not delete it and do not expose it as the firmware RPC API.


### Exact pinned-source audit from CI

CI run 52 checked out the gitlink SHA `55873baa1b37eaf7d0a830515a0846e3fc3a79e2` and inspected the actual source files from that revision.

The exact CMake/API findings are:

- `pw::rpc::ChannelOutput` is declared in `pw_rpc/public/pw_rpc/channel.h`; there is no separate `channel_output.h` at this revision.
- The relevant upstream targets are `pw_rpc.common`, `pw_rpc.server`, and `pw_rpc.raw.server_api`. The raw server target depends on `pw_rpc.server` and `pw_bytes`.
- The server endpoint accepts a static channel span and exposes `Server::ProcessPacket(ConstByteSpan)`.
- `pw_rpc.common` depends on `pw_sync.lock_annotations`; it conditionally links `pw_sync.mutex` only when a mutex backend is configured. Its configuration defaults `PW_RPC_USE_GLOBAL_MUTEX` to 1.
- The RPC output contract requires `Send(span<const std::byte>)` to transmit or copy the packet before returning. The output callback must not call back into any RPC API.
- The `pw_transfer.core` target includes `transfer_thread.cc` and publicly depends on `pw_rpc.client`, both raw RPC APIs, `pw_sync.binary_semaphore`, `pw_sync.timed_thread_notification`, and `pw_thread.thread_core`.
- There is no cooperative transfer target in this pinned CMake graph. A plain link to `pw_transfer.core` would retain the thread/semaphore execution model.

Consequences for the implementation:

1. Integrate upstream RPC as a standalone server-only target first, with static channels and a bounded UART/HDLC egress path.
2. Do not pull in `pw_rpc.client`, RPC test helpers, or threaded test targets for the initial server-only proof.
3. Do not link `pw_transfer.core` into AVR firmware unchanged. The cooperative transfer work must refactor the upstream event/context implementation while preserving the protocol state machines and the normal threaded build.
4. The exact `pw_rpc` lock configuration and the pinned `pw_hdlc` RPC transport API must be validated by the AVR compiler before the firmware target is changed.

CI run 52 passed the existing AVR build and simavr suite while performing this source inspection. It did not compile or link upstream RPC; that remains a separate gate.
