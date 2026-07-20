#pragma once

#include <cstdint>

namespace core {

// Arduino-free trusted-UTC policy and non-blocking state machine. The ESP
// service adapter (services::timekeeper) supplies the SNTP samples, the Wi-Fi
// link state, and millis(), and performs the side effects (configTime, NVS);
// all of the trust/rejection/floor decisions live here so they are unit-testable
// without hardware. See README "Trusted time" for the rationale behind the
// constants the adapter passes in.

// Policy the adapter builds from config.h. Kept as an explicit struct (never
// read from config.h) so the core stays Arduino-free, exactly like
// core::AdsbPollPolicy.
struct TimeTrustPolicy {
  int64_t release_floor_unix;         // committed release/build epoch floor
  int64_t rollback_tolerance_s;       // a sample may sit at most this far BELOW floor
  int64_t future_ceiling_s;           // a sample may sit at most this far ABOVE floor
  uint32_t sync_timeout_ms;           // SyncPending -> Retry when no sample arrives
  uint32_t retry_backoff_initial_ms;  // first Retry wait
  uint32_t retry_backoff_max_ms;      // Retry wait ceiling (doubles toward it)
  uint32_t trusted_sample_max_age_ms; // Trusted -> revoke+resync once the accepted
                                      // sample is older than this (finite; well
                                      // below the uint32 millis wrap so the
                                      // rollover-safe elapsed stays exact)
};

// Parameters for the authenticated, per-24h persisted-floor ratchet. Separate
// from TimeTrustPolicy because they only shape the flash-persistence throttle,
// not the trust decision. The adapter builds this from config.h. The ratchet's
// candidate is the CA-signed peer leaf notBefore epoch (never SNTP-derived time),
// so an unauthenticated NTP attacker cannot choose the value being persisted.
struct FloorRatchetPolicy {
  int64_t release_floor_unix;  // candidate must be >= this plausible-epoch lower bound
  int64_t future_ceiling_s;    // candidate must be <= release_floor_unix + this (finite)
  int64_t min_advance_s;       // candidate must exceed the current floor by >= this (24 h)
  uint32_t min_interval_ms;    // in-session flash-wear guard (rollover-safe millis)
};

// --- Sample classification (pure) -------------------------------------------

enum class TimeSampleClass : uint8_t {
  Accept,
  RejectBelowFloor,    // sample < floor - rollback_tolerance (stale / rollback)
  RejectAboveCeiling,  // sample > floor + future_ceiling (implausible future)
};

// Effective lower bound for accepting a time sample: the greater of the release
// floor and a validated persisted floor. Never below the release floor.
int64_t effectiveTimeFloorUnix(const TimeTrustPolicy& policy,
                               int64_t persisted_floor_unix);

// Classify a sample against the inclusive window
// [floor - rollback_tolerance_s, floor + future_ceiling_s].
TimeSampleClass classifyTimeSample(int64_t sample_unix, int64_t floor_unix,
                                   const TimeTrustPolicy& policy);

// Validate a raw persisted floor read from NVS. Accepts it only when it is a
// plausible epoch: >= release_floor (a lower value is stale/rollback and is
// ignored) and <= release_floor + future_ceiling (a corrupt/implausibly-future
// value is ignored -- which also stops a corrupt NVS value from raising the
// floor so high that no genuine sample is ever accepted). On acceptance sets
// *out_floor_unix and returns true; otherwise returns false and the caller keeps
// the release floor. This NEVER weakens the release floor.
bool validatePersistedFloor(const TimeTrustPolicy& policy, uint64_t raw_nvs_value,
                            int64_t* out_floor_unix);

// --- Versioned, integrity-checked persisted-floor record (NVS blob) ----------

// A single NVS blob carries ONLY the CA-authenticated monotonic floor (the last
// persisted peer-leaf notBefore epoch). Because the floor advances solely by a
// CA-signed certificate notBefore -- never by an attacker-choosable SNTP sample
// -- no auxiliary "last write epoch" is required: replaying the same certificate
// yields the same notBefore, which cannot advance the stored floor. Layout is a
// fixed 16-byte little-endian record so the encoding is deterministic and
// unit-testable off hardware:
//   [0..3]   version   (uint32, == kPersistedFloorVersion)
//   [4..11]  floor_unix (int64, the CA-authenticated monotonic lower bound)
//   [12..15] checksum  (uint32 FNV-1a over bytes [0..11])
// A corrupt, partial, wrong-version, or future-invalid record fails validation
// and the caller falls back to the release floor (never weakening it).
constexpr uint32_t kPersistedFloorVersion = 2;
constexpr uint32_t kPersistedFloorRecordBytes = 16;

struct PersistedFloorRecord {
  int64_t floor_unix;  // CA-authenticated monotonic lower bound (release..release+ceiling)
};

// Serialize a record into exactly kPersistedFloorRecordBytes bytes (checksum
// included). Returns false if buf is too small.
bool encodePersistedFloor(const PersistedFloorRecord& rec, uint8_t* buf,
                          uint32_t len);

// Parse + fully validate a persisted record: the length must be exactly
// kPersistedFloorRecordBytes, the version and FNV-1a checksum must match, and the
// floor must be within [release, release + future_ceiling]. On success sets *out
// and returns true; on ANY failure returns false (caller keeps the release
// floor). Pure and Arduino-free.
bool decodeAndValidatePersistedFloor(const TimeTrustPolicy& policy,
                                     const uint8_t* buf, uint32_t len,
                                     PersistedFloorRecord* out);

// --- Non-blocking trust state machine ---------------------------------------

enum class TimeTrustPhase : uint8_t {
  WaitingForWifi,  // no usable link yet; cannot obtain a sample
  SyncPending,     // link up, SNTP armed, awaiting a valid sample
  Trusted,         // a valid sample accepted THIS boot; UTC is trusted
  Retry,           // a sample was rejected or sync timed out; backing off
};

struct TimeTrustState {
  TimeTrustPhase phase;
  int64_t persisted_floor_unix;  // validated persisted lower bound (>= release)
  int64_t trusted_unix;          // last ACCEPTED sample epoch (the anchor; valid when Trusted)
  uint32_t accepted_ms;          // millis() when trusted_unix was accepted (monotonic anchor)
  uint32_t phase_started_ms;     // millis the current SyncPending/Retry wait began
  uint32_t retry_backoff_ms;     // current Retry wait; doubles toward the cap
  bool floor_write_attempted;    // a persist write happened this session
  uint32_t last_floor_write_ms;  // millis of the last persist attempt
};

struct TimeTrustInputs {
  bool wifi_up;
  bool sample_available;  // a fresh latched SNTP sample is present this tick
  int64_t sample_unix;    // meaningful only when sample_available
  uint32_t now_ms;
};

struct TimeTrustActions {
  bool start_sntp;  // adapter should (re)arm SNTP (configTime / sntp_restart)
};

// Initialize an untrusted state for a fresh boot. EVERY boot starts untrusted
// (phase WaitingForWifi) regardless of the persisted floor -- the persisted
// value only raises the acceptance floor, it never confers trust. The passed
// persisted_floor_unix is clamped to at least the release floor.
void timeTrustInit(TimeTrustState* state, const TimeTrustPolicy& policy,
                   int64_t persisted_floor_unix);

// Advance the state machine one non-blocking tick. All millis() comparisons use
// rollover-safe deltas. Rules:
//   * An accepted sample older than trusted_sample_max_age_ms can no longer be
//     trusted (derived time has drifted too far): revoke trust and re-arm SNTP
//     (or wait for Wi-Fi), regardless of link state. This is checked FIRST.
//   * Link down while Trusted (and fresh): stay Trusted (the derived clock keeps
//     advancing); a brief drop never revokes trust.
//   * Link down while not Trusted: WaitingForWifi.
//   * Link up from WaitingForWifi: enter SyncPending and request start_sntp.
//   * A fresh sample (any online phase, including a Trusted resync): Accept ->
//     Trusted (re-anchors accepted_ms); a rejected sample -> Retry (revoking
//     trust if it was Trusted).
//   * SyncPending with no sample past sync_timeout_ms -> Retry.
//   * Retry past the current backoff -> SyncPending (request start_sntp); the
//     backoff then doubles toward retry_backoff_max_ms.
TimeTrustActions timeTrustStep(TimeTrustState* state,
                               const TimeTrustPolicy& policy,
                               const TimeTrustInputs& in);

// True only in the Trusted phase. (Does NOT re-check max-age; use
// derivedTrustedNowUnix for a staleness-aware value at a specific millis.)
bool timeTrustIsTrusted(const TimeTrustState& state);

// Pure DERIVED trusted-UTC clock: the accepted sample epoch plus the rollover-
// safe elapsed millis since it was accepted (never the mutable system wall
// clock). Returns false -- leaving *out untouched -- when not Trusted OR when the
// accepted sample is already older than trusted_sample_max_age_ms (stale), so
// every security decision (nowUnix, cert notBefore/notAfter, floor ratchet) fails
// closed on stale/absent time. Deterministic and unit-testable off hardware.
bool derivedTrustedNowUnix(const TimeTrustState& state,
                           const TimeTrustPolicy& policy, uint32_t now_ms,
                           int64_t* out);

// Defense-in-depth fetch gate: an ADS-B fetch may start only when the poll
// schedule says it is due AND trusted UTC has been established this boot. Pure so
// the "never fetch before trust; fetch immediately once trusted" rule is
// unit-testable. The caller MUST NOT advance any poll/latch state when this
// returns false, so a pending immediate-fetch latch survives untrusted time and
// fires on the first loop after trust.
inline bool adsbFetchAllowed(bool poll_due, bool time_trusted) {
  return poll_due && time_trusted;
}

// --- Persisted-floor ratchet (CA-authenticated) ------------------------------

// Decide whether a verified-fetch ratchet should persist a new floor, and what
// value to write. The candidate is the CA-signed peer leaf notBefore epoch from a
// COMPLETE, CA+hostname+date verified ADS-B response (the caller enforces that
// precondition and passes 0 for any non-Ok / unauthenticated path). All of these
// must hold:
//   * Plausible authenticated epoch: candidate in
//     [release_floor_unix, release_floor_unix + future_ceiling_s]. A zero or
//     below-release value (unverified / stale / rollback) and an implausibly
//     far-future value (corrupt / mis-issued cert) are rejected. This also keeps
//     the written floor inside the range decodeAndValidatePersistedFloor accepts.
//   * Authenticated advancement: candidate advances the current floor by at least
//     policy.min_advance_s (24 h). A same/older/concurrently-served alternate
//     certificate (notBefore <= floor) or one less than min_advance_s newer does
//     NOT write, so it can neither ratchet repeatedly nor roll the floor back.
//     Because the candidate is CA-signed, spoofing SNTP time cannot satisfy this.
//   * In-session flash-wear guard: at most one write per policy.min_interval_ms
//     (rollover-safe millis); the first write of a session skips this gate. This
//     is an in-RAM backstop only -- it is NOT a cross-reboot time throttle.
// The persisted value is the EXACT authenticated notBefore candidate (never a
// capped SNTP value); it is guaranteed strictly above the current floor by the
// advancement rule. Sets *out_value and returns true when a write should occur.
bool shouldRatchetPersistedFloor(const TimeTrustState& state,
                                 int64_t candidate_not_before_unix,
                                 uint32_t now_ms,
                                 const FloorRatchetPolicy& policy,
                                 uint64_t* out_value);

// Record a persist attempt (whether or not the NVS write succeeded) so the
// in-session flash-wear guard is honored consistently. Only a successful write
// advances the in-RAM persisted floor, and only as a monotonic maximum (a write
// can never roll the floor back); a failed write leaves it unchanged (never
// weakening trust) and simply defers the next attempt by the interval.
void noteFloorWrite(TimeTrustState* state, uint64_t written_floor,
                    uint32_t now_ms, bool success);

}  // namespace core
