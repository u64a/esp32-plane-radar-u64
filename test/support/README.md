# Native test support

These helpers stay independent of Arduino and production headers:

- `ManualClock` supplies explicit, rollover-preserving `uint32_t` time.
- `SequenceRng` supplies a finite deterministic value sequence and fails on exhaustion.
- `CapturedOutput` records text without emulating Arduino `Serial`.

Settings and transport doubles are intentionally deferred until their Phase 5 and 6
production contracts exist. Adding them now would invent storage, TLS, streaming, and
failure semantics that the tests should instead inherit from reviewed interfaces.
