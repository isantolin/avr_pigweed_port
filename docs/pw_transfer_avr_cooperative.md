# Cooperative upstream `pw_transfer` on AVR

## Decision

The AVR port uses **upstream Pigweed `pw_rpc` and `pw_transfer` only**. The repository-specific `pw_rpc_avr` dispatcher has been removed: it is not built, tested, exported, or retained as a fallback.

The MCU transport remains UART + upstream `pw_hdlc`. RPC packets are carried by `pw_hdlc::RpcChannelOutput` and dispatched by `pw::rpc::Server` / `pw::rpc::Client`. File/data transfer uses upstream `pw_transfer::TransferService` and `pw_transfer::Client` over those same RPC channels.

Pinned Pigweed revision for this implementation:

`55873baa1b37eaf7d0a830515a0846e3fc3a79e2`

## AVR constraints

The implementation must remain suitable for bare-metal AVR:

- No operating-system threads and no blocking transfer-thread run loop.
- No heap allocation in the transfer event path.
- Fixed-size event and chunk storage.
- Explicit processing budget in the MCU main loop.
- One active transfer role per `TransferThread`: client **or** server, never both at once.
- The same `Thread<1, 1>` object can be reused for client and server work sequentially after the previous transfer releases its role.

`Thread<1, 1>` describes one client context and one server context in the upstream API; it does **not** mean that both roles may execute concurrently. The AVR cooperative role arbiter enforces the stricter single-active-role rule.

## Cooperative event processing

The AVR patch is applied to the exact pinned Pigweed source by `tools/ci/apply_pigweed_patches.py`. The patch is idempotent and is guarded by the upstream source signatures.

The cooperative implementation provides:

- `Status Poll(size_t budget = 1)`: process no more than the supplied number of queued events, then check transfer timeouts.
- A fixed ring queue, default capacity **2**.
- Per-slot fixed chunk storage, default **128 bytes**, so event payload pointers do not outlive the upstream staging buffer.
- `Status::Unavailable()` when accepted events remain after the processing budget.
- `Status::ResourceExhausted()` when the bounded queue overflows or an event payload exceeds its slot storage.
- Explicit rejection of a second pending stream mutation and a second pending resource-status callback rather than overwriting staged upstream state.
- A client/server role arbiter keyed by role and session ID. Retransmissions for the active session remain valid; a conflicting role/session is rejected.
- Timeout processing through upstream `HandleTimeouts()` from `Poll()`.

On overflow, accepted events are processed in bounded order, pending callbacks are completed with an error where possible, active transfers are aborted with `ResourceExhausted`, and cooperative role/staging state is reset. The implementation must fail the transfer rather than silently lose a chunk or corrupt an upstream stream.

The MCU main loop is responsible for calling `Poll(budget)` regularly. It must not call the blocking upstream `Run()` path.

## RPC and transfer validation

The port contains two different probes because the RPC endpoint and transfer endpoint have different resource requirements.

### Upstream RPC probe

`tests/pw_rpc_upstream_probe.cpp` exercises:

- Upstream `pw::rpc::Server` and `pw::rpc::Client`.
- Unary request/response.
- Bidirectional raw streaming.
- Real HDLC encode/decode loopback between the client and server channel outputs.
- Generated service registration and response completion.

The probe is built for ATmega328P. Its UART result marker is emitted only after all runtime assertions pass.

### Upstream transfer probe

`tests/pw_transfer_upstream_probe.cpp` exercises:

- A real upstream `pw_transfer::TransferService` and `pw_transfer::Client`.
- A complete read transfer and a complete write transfer.
- HDLC framing and upstream RPC packet dispatch in both directions.
- Cooperative event polling with a bounded processing budget.
- Resource-status callback staging and duplicate rejection.
- Sequential reuse of the same two transfer-thread objects with their roles reversed: the first endpoint starts as client and then becomes server, while the second starts as server and then becomes client.

The loopback probe uses separate `Thread<1, 1>` instances for the two simulated endpoints. That is intentional: one endpoint is a client and the other is a server during a transfer. A single AVR `Thread<1, 1>` is not permitted to run both roles concurrently.

The end-to-end transfer probe is built for ATmega2560 to provide enough SRAM for both simulated endpoints. A separate shared-thread footprint probe is built for ATmega32U4, instantiating the production arrangement (one RPC channel, both RPC endpoints, one `Thread<1, 1>`, and both transfer service/client objects). The complete Bridge firmware footprint, worst-case stack, and hardware UART/HDLC validation remain release gates.

CI runs both `PW_CHECKSUM_CRC32_1BITS` and `PW_CHECKSUM_CRC32_4BITS`. The firmware UART pass marker, rather than simavr's terminal process status after firmware shutdown, is the runtime-probe result.

## Arduino library export

The Arduino export must contain:

- Upstream `pw_rpc` server **and client** sources and raw RPC interfaces.
- Upstream `pw_transfer` public headers and implementation sources.
- The generated `pw_transfer` PWPB and raw-RPC headers from the CMake build.
- The AVR cooperative compile definitions: `PW_TRANSFER_COOPERATIVE=1`, queue capacity 2, chunk storage 128 bytes.
- The upstream HDLC/RPC headers exposed by `PigweedAVR.h`.

The exporter refuses to package `pw_transfer` if the pinned-source cooperative patch or generated protocol headers are missing. This prevents exporting a header-only API that cannot be linked or a blocking upstream thread implementation that is unsuitable for AVR.

## Bridge integration contract

The port provides the transport and transfer implementation; the Bridge firmware still owns its application resource handlers. The Bridge should:

1. Own one statically allocated `pw::transfer::Thread<1, 1>` and call `Poll(budget)` from its cooperative main loop.
2. Register upstream `TransferService` handlers for resources that the MPU is allowed to read or write.
3. Use the upstream `pw_transfer::Client` for MCU-initiated transfers.
4. Never start a client transfer while a server transfer is active on that same thread.
5. Keep existing unary RPCs for small control operations; use `pw_transfer` for bounded, chunked file/data movement where its retry, timeout, and flow-control semantics add value.

The existing `FileSystemService.Read` server stream is not automatically equivalent to `pw_transfer`. Migrating it requires matching resource IDs, read/write handler lifetimes, MPU-side transfer support, and end-to-end tests. It must not be silently replaced by a second, uncoordinated file-transfer path.

## Remaining release gates

- Complete CI for both CRC configurations after the latest source/export changes.
- Verify the exported Arduino library links a sketch that uses upstream `pw_transfer`.
- Measure flash, static SRAM, and worst-case stack on ATmega32U4 (the Bridge target), not only ATmega328P/ATmega2560 probes.
- Exercise sequential client-to-server and server-to-client role reuse on one `Thread<1, 1>`.
- Add cancellation, timeout, retry, and queue-saturation runtime tests.
- Integrate the validated port revision into `arduino-yun-bridge2`, then test the MCU and MPU implementations together over the real UART/HDLC transport.

No production-readiness claim should be made until those gates pass.
