#include "core/time_trust.h"

#include "core/time_math.h"

namespace core {

namespace {

uint32_t doubleBackoff(uint32_t current, const TimeTrustPolicy& policy) {
  if (current >= policy.retry_backoff_max_ms) {
    return policy.retry_backoff_max_ms;
  }
  const uint64_t doubled = static_cast<uint64_t>(current) * 2U;
  if (doubled >= policy.retry_backoff_max_ms) {
    return policy.retry_backoff_max_ms;
  }
  return static_cast<uint32_t>(doubled);
}

// Little-endian (de)serialization helpers so the persisted record encodes
// identically on the ESP32-C3 and on the native test host, independent of the
// build platform's own endianness.
void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void putI64(uint8_t* p, int64_t v) {
  const uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFFu);
  }
}

int64_t getI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) {
    u |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return static_cast<int64_t>(u);
}

// FNV-1a 32-bit: a small deterministic integrity check that detects corrupt or
// partially-written NVS blobs. It is NOT a cryptographic MAC (the record is not a
// trust anchor -- it only carries a monotonic lower bound and a write timestamp,
// both re-validated against the release floor on read).
uint32_t fnv1a32(const uint8_t* data, uint32_t len) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace

int64_t effectiveTimeFloorUnix(const TimeTrustPolicy& policy,
                               int64_t persisted_floor_unix) {
  return persisted_floor_unix > policy.release_floor_unix ? persisted_floor_unix
                                                          : policy.release_floor_unix;
}

TimeSampleClass classifyTimeSample(int64_t sample_unix, int64_t floor_unix,
                                   const TimeTrustPolicy& policy) {
  if (sample_unix < floor_unix - policy.rollback_tolerance_s) {
    return TimeSampleClass::RejectBelowFloor;
  }
  if (sample_unix > floor_unix + policy.future_ceiling_s) {
    return TimeSampleClass::RejectAboveCeiling;
  }
  return TimeSampleClass::Accept;
}

bool validatePersistedFloor(const TimeTrustPolicy& policy, uint64_t raw_nvs_value,
                            int64_t* out_floor_unix) {
  const int64_t value = static_cast<int64_t>(raw_nvs_value);
  // A value below the release floor is stale/rollback; a value above the future
  // ceiling is corrupt or implausibly future. Both are ignored so neither can
  // weaken the release floor nor lock out every genuine sample.
  if (value < policy.release_floor_unix) {
    return false;
  }
  if (value > policy.release_floor_unix + policy.future_ceiling_s) {
    return false;
  }
  *out_floor_unix = value;
  return true;
}

bool encodePersistedFloor(const PersistedFloorRecord& rec, uint8_t* buf,
                          uint32_t len) {
  if (buf == nullptr || len < kPersistedFloorRecordBytes) {
    return false;
  }
  putU32(buf + 0, kPersistedFloorVersion);
  putI64(buf + 4, rec.floor_unix);
  putU32(buf + 12, fnv1a32(buf, 12));  // checksum over version + floor
  return true;
}

bool decodeAndValidatePersistedFloor(const TimeTrustPolicy& policy,
                                     const uint8_t* buf, uint32_t len,
                                     PersistedFloorRecord* out) {
  // Exact length only: a partial or oversized blob is corrupt.
  if (buf == nullptr || len != kPersistedFloorRecordBytes) {
    return false;
  }
  if (getU32(buf + 0) != kPersistedFloorVersion) {
    return false;
  }
  if (getU32(buf + 12) != fnv1a32(buf, 12)) {
    return false;  // integrity failure (corrupt / torn write)
  }
  // The floor must be a plausible epoch in [release, release + ceiling]; reuse
  // the single floor-window validator (a value below release is stale/rollback
  // and one above the ceiling is future-invalid).
  int64_t floor = 0;
  if (!validatePersistedFloor(policy, static_cast<uint64_t>(getI64(buf + 4)),
                              &floor)) {
    return false;
  }
  out->floor_unix = floor;
  return true;
}

void timeTrustInit(TimeTrustState* state, const TimeTrustPolicy& policy,
                   int64_t persisted_floor_unix) {
  *state = TimeTrustState{};
  state->phase = TimeTrustPhase::WaitingForWifi;
  state->persisted_floor_unix =
      persisted_floor_unix > policy.release_floor_unix ? persisted_floor_unix
                                                       : policy.release_floor_unix;
  state->trusted_unix = 0;
  state->accepted_ms = 0;
  state->phase_started_ms = 0;
  state->retry_backoff_ms = policy.retry_backoff_initial_ms;
  state->floor_write_attempted = false;
  state->last_floor_write_ms = 0;
}

TimeTrustActions timeTrustStep(TimeTrustState* state,
                               const TimeTrustPolicy& policy,
                               const TimeTrustInputs& in) {
  TimeTrustActions actions{false};

  // Staleness revoke FIRST: an accepted sample older than the max age can no
  // longer be trusted (the derived clock has drifted too far from real UTC),
  // regardless of link state. Fail closed and re-sync.
  if (state->phase == TimeTrustPhase::Trusted &&
      elapsedAtLeast(in.now_ms, state->accepted_ms,
                     policy.trusted_sample_max_age_ms)) {
    if (in.wifi_up) {
      state->phase = TimeTrustPhase::SyncPending;
      state->phase_started_ms = in.now_ms;
      state->retry_backoff_ms = policy.retry_backoff_initial_ms;
      actions.start_sntp = true;  // re-arm SNTP to obtain a fresh sample
    } else {
      state->phase = TimeTrustPhase::WaitingForWifi;
    }
    // Fall through: a fresh sample arriving on this same tick can still be
    // consumed below and re-establish trust immediately.
  }

  // Link down: Trusted (and still fresh) survives; otherwise wait.
  if (!in.wifi_up) {
    if (state->phase != TimeTrustPhase::Trusted) {
      state->phase = TimeTrustPhase::WaitingForWifi;
    }
    return actions;
  }

  // Link up: leave WaitingForWifi by arming a fresh sync.
  if (state->phase == TimeTrustPhase::WaitingForWifi) {
    state->phase = TimeTrustPhase::SyncPending;
    state->phase_started_ms = in.now_ms;
    state->retry_backoff_ms = policy.retry_backoff_initial_ms;
    actions.start_sntp = true;
  }

  // A fresh sample takes precedence in any online phase (including a Trusted
  // resync: a later rejected sample revokes trust).
  if (in.sample_available) {
    const int64_t floor =
        effectiveTimeFloorUnix(policy, state->persisted_floor_unix);
    if (classifyTimeSample(in.sample_unix, floor, policy) ==
        TimeSampleClass::Accept) {
      state->phase = TimeTrustPhase::Trusted;
      state->trusted_unix = in.sample_unix;
      state->accepted_ms = in.now_ms;  // re-anchor the derived monotonic clock
      state->retry_backoff_ms = policy.retry_backoff_initial_ms;
    } else {
      state->phase = TimeTrustPhase::Retry;
      state->phase_started_ms = in.now_ms;
    }
    return actions;
  }

  // No new sample this tick: handle sync timeout / retry backoff by phase.
  if (state->phase == TimeTrustPhase::SyncPending) {
    if (elapsedAtLeast(in.now_ms, state->phase_started_ms, policy.sync_timeout_ms)) {
      state->phase = TimeTrustPhase::Retry;
      state->phase_started_ms = in.now_ms;
    }
  } else if (state->phase == TimeTrustPhase::Retry) {
    if (elapsedAtLeast(in.now_ms, state->phase_started_ms, state->retry_backoff_ms)) {
      state->phase = TimeTrustPhase::SyncPending;
      state->phase_started_ms = in.now_ms;
      state->retry_backoff_ms = doubleBackoff(state->retry_backoff_ms, policy);
      actions.start_sntp = true;
    }
  }
  // Trusted with no sample and still fresh: remain trusted.
  return actions;
}

bool timeTrustIsTrusted(const TimeTrustState& state) {
  return state.phase == TimeTrustPhase::Trusted;
}

bool derivedTrustedNowUnix(const TimeTrustState& state,
                           const TimeTrustPolicy& policy, uint32_t now_ms,
                           int64_t* out) {
  if (state.phase != TimeTrustPhase::Trusted) {
    return false;  // no accepted sample this boot
  }
  const uint32_t elapsed_ms = elapsedMs(now_ms, state.accepted_ms);
  if (elapsed_ms >= policy.trusted_sample_max_age_ms) {
    return false;  // stale: derived time has drifted too far, fail closed
  }
  *out = state.trusted_unix + static_cast<int64_t>(elapsed_ms / 1000U);
  return true;
}

bool shouldRatchetPersistedFloor(const TimeTrustState& state,
                                 int64_t candidate_not_before_unix,
                                 uint32_t now_ms,
                                 const FloorRatchetPolicy& policy,
                                 uint64_t* out_value) {
  // Plausible authenticated epoch: the candidate is the CA-signed peer leaf
  // notBefore. Reject a value below the release floor (a zero/unverified field, a
  // stale or rolled-back cert) or above the finite future ceiling (a corrupt or
  // mis-issued far-future cert). This also keeps the written floor inside the
  // range decodeAndValidatePersistedFloor accepts on the next boot.
  if (candidate_not_before_unix < policy.release_floor_unix) {
    return false;
  }
  if (candidate_not_before_unix >
      policy.release_floor_unix + policy.future_ceiling_s) {
    return false;
  }
  // Authenticated advancement throttle: the candidate must advance the current
  // floor by at least min_advance_s (24 h). A same/older/concurrently-served
  // alternate certificate (notBefore <= floor) or one less than min_advance_s
  // newer does NOT write, so it can neither ratchet repeatedly across reboots nor
  // roll the floor back. Because the candidate is CA-signed, an unauthenticated
  // NTP attacker cannot satisfy this by spoofing SNTP time.
  if (candidate_not_before_unix <
      state.persisted_floor_unix + policy.min_advance_s) {
    return false;
  }
  // In-session flash-wear guard; the first write of a session is unthrottled.
  // In-RAM only: this is NOT a cross-reboot time throttle.
  if (state.floor_write_attempted &&
      !elapsedAtLeast(now_ms, state.last_floor_write_ms, policy.min_interval_ms)) {
    return false;
  }
  // Write the EXACT authenticated notBefore (never a capped SNTP value). The
  // advancement rule above guarantees it is strictly above the current floor.
  *out_value = static_cast<uint64_t>(candidate_not_before_unix);
  return true;
}

void noteFloorWrite(TimeTrustState* state, uint64_t written_floor,
                    uint32_t now_ms, bool success) {
  state->floor_write_attempted = true;
  state->last_floor_write_ms = now_ms;
  if (success) {
    const int64_t value = static_cast<int64_t>(written_floor);
    if (value > state->persisted_floor_unix) {  // monotonic max: never roll back
      state->persisted_floor_unix = value;
    }
  }
}

}  // namespace core
