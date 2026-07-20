# Native test support

These helpers stay independent of Arduino and production headers:

- `ManualClock` supplies explicit, rollover-preserving `uint32_t` time.
- `SequenceRng` supplies a finite deterministic value sequence and fails on exhaustion.
- `CapturedOutput` records text without emulating Arduino `Serial`.
- `adsb_stream_fakes.h` scripts the Phase 5 Arduino-free transport seams: a
  `ScriptedByteSource` (any fragmentation, WouldBlock injection, clean end /
  transport error / stall terminals), a `CountingClock`, and an `AdvancingIdle`
  pump for deterministic deadline and back-pressure tests.

Settings doubles remain deferred until their Phase 6 production contracts exist.
Adding them now would invent storage and failure semantics that the tests should
instead inherit from reviewed interfaces.
