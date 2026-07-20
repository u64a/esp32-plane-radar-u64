#include <unity.h>

#include <cstdint>
#include <cstring>

#include "core/time_trust.h"

using core::FloorRatchetPolicy;
using core::PersistedFloorRecord;
using core::TimeSampleClass;
using core::TimeTrustActions;
using core::TimeTrustInputs;
using core::TimeTrustPhase;
using core::TimeTrustPolicy;
using core::TimeTrustState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// Small, boundary-friendly policy. floor=1e6, rollback=100, ceiling=10000,
// sync timeout=1000 ms, retry backoff 500 ms doubling to a 4000 ms cap, and a
// 12000 ms accepted-sample max age (stand-in for the firmware's 12 h).
constexpr int64_t kRelease = 1000000;
constexpr int64_t kRollback = 100;
constexpr int64_t kCeiling = 10000;
constexpr uint32_t kSyncTimeout = 1000;
constexpr uint32_t kBackoffInit = 500;
constexpr uint32_t kBackoffMax = 4000;
constexpr uint32_t kMaxAge = 12000;

constexpr TimeTrustPolicy P{kRelease,     kRollback,    kCeiling, kSyncTimeout,
                            kBackoffInit, kBackoffMax,  kMaxAge};

// Ratchet policy: the CA-signed candidate must sit in [kRelease, kRelease +
// kCeiling], advance the stored floor by >= 1000 s (the firmware's 24 h analog),
// and honor a 10000 ms in-session flash-wear guard. There is NO cross-reboot time
// throttle -- security comes from the authenticated candidate vs. the stored
// floor, not from any SNTP timestamp.
constexpr FloorRatchetPolicy RP{/*release_floor_unix=*/kRelease,
                                /*future_ceiling_s=*/kCeiling,
                                /*min_advance_s=*/1000,
                                /*min_interval_ms=*/10000};

int ph(TimeTrustPhase p) { return static_cast<int>(p); }
int sc(TimeSampleClass c) { return static_cast<int>(c); }

TimeTrustActions step(TimeTrustState* s, bool wifi, bool sample, int64_t sunix,
                      uint32_t now) {
  TimeTrustInputs in{wifi, sample, sunix, now};
  return core::timeTrustStep(s, P, in);
}

TimeTrustState trustedState() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  step(&s, true, false, 0, 0);              // WaitingForWifi -> SyncPending
  step(&s, true, true, kRelease + 1000, 10);  // accept -> Trusted
  return s;
}

// Simulate a reboot: a fresh boot state seeded from the persisted floor, then
// driven to Trusted by accepting `spoofed_now` as an SNTP sample. The SNTP value
// is deliberately attacker-chosen; the ratchet must ignore it and use ONLY the
// CA-signed cert notBefore passed to shouldRatchetPersistedFloor.
TimeTrustState rebootTrusted(int64_t persisted_floor, int64_t spoofed_now,
                             uint32_t now_ms) {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, persisted_floor);
  step(&s, true, false, 0, now_ms);
  step(&s, true, true, spoofed_now, now_ms + 1);
  return s;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- floor selection ---------------------------------------------------------

void test_effective_floor_selects_max_of_release_and_persisted() {
  TEST_ASSERT_EQUAL_INT64(kRelease,
                          core::effectiveTimeFloorUnix(P, kRelease - 5000));
  TEST_ASSERT_EQUAL_INT64(kRelease, core::effectiveTimeFloorUnix(P, kRelease));
  TEST_ASSERT_EQUAL_INT64(kRelease + 5000,
                          core::effectiveTimeFloorUnix(P, kRelease + 5000));
}

// --- sample classification boundaries ---------------------------------------

void test_sample_classification_exact_boundaries() {
  const int64_t floor = kRelease;  // effective floor when persisted==release
  // Lower tolerance boundary is inclusive.
  TEST_ASSERT_EQUAL_INT(
      sc(TimeSampleClass::RejectBelowFloor),
      sc(core::classifyTimeSample(floor - kRollback - 1, floor, P)));
  TEST_ASSERT_EQUAL_INT(sc(TimeSampleClass::Accept),
                        sc(core::classifyTimeSample(floor - kRollback, floor, P)));
  TEST_ASSERT_EQUAL_INT(sc(TimeSampleClass::Accept),
                        sc(core::classifyTimeSample(floor, floor, P)));
  // Future ceiling boundary is inclusive.
  TEST_ASSERT_EQUAL_INT(sc(TimeSampleClass::Accept),
                        sc(core::classifyTimeSample(floor + kCeiling, floor, P)));
  TEST_ASSERT_EQUAL_INT(
      sc(TimeSampleClass::RejectAboveCeiling),
      sc(core::classifyTimeSample(floor + kCeiling + 1, floor, P)));
}

// --- persisted-floor NVS validation -----------------------------------------

void test_validate_persisted_floor_corrupt_older_future() {
  int64_t out = -1;
  // Valid: within [release, release + ceiling].
  TEST_ASSERT_TRUE(core::validatePersistedFloor(
      P, static_cast<uint64_t>(kRelease + 5000), &out));
  TEST_ASSERT_EQUAL_INT64(kRelease + 5000, out);
  TEST_ASSERT_TRUE(
      core::validatePersistedFloor(P, static_cast<uint64_t>(kRelease), &out));
  TEST_ASSERT_EQUAL_INT64(kRelease, out);
  TEST_ASSERT_TRUE(core::validatePersistedFloor(
      P, static_cast<uint64_t>(kRelease + kCeiling), &out));  // upper boundary
  TEST_ASSERT_EQUAL_INT64(kRelease + kCeiling, out);

  // Rejected (older than release / rollback): floor unchanged, out untouched.
  out = 777;
  TEST_ASSERT_FALSE(core::validatePersistedFloor(
      P, static_cast<uint64_t>(kRelease - 1), &out));
  TEST_ASSERT_EQUAL_INT64(777, out);
  // Rejected (implausibly future, just over the ceiling).
  TEST_ASSERT_FALSE(core::validatePersistedFloor(
      P, static_cast<uint64_t>(kRelease + kCeiling + 1), &out));
  TEST_ASSERT_EQUAL_INT64(777, out);
  // Rejected (corrupt zero and corrupt huge value that casts negative).
  TEST_ASSERT_FALSE(core::validatePersistedFloor(P, 0, &out));
  TEST_ASSERT_FALSE(core::validatePersistedFloor(P, UINT64_MAX, &out));
  TEST_ASSERT_EQUAL_INT64(777, out);
}

// --- every boot starts untrusted, even with a persisted floor ---------------

void test_boot_starts_untrusted_even_with_persisted_floor() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease + 5000);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::WaitingForWifi), ph(s.phase));
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_EQUAL_INT64(kRelease + 5000, s.persisted_floor_unix);

  // A persisted value below the release floor is clamped up (never weakens it).
  TimeTrustState s2{};
  core::timeTrustInit(&s2, P, kRelease - 9999);
  TEST_ASSERT_EQUAL_INT64(kRelease, s2.persisted_floor_unix);
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s2));
}

// --- basic online transitions -----------------------------------------------

void test_wifi_and_sample_transitions() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  // Link still down: stays WaitingForWifi.
  step(&s, false, false, 0, 0);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::WaitingForWifi), ph(s.phase));
  // Link up: SyncPending, request SNTP arm.
  TimeTrustActions a = step(&s, true, false, 0, 5);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::SyncPending), ph(s.phase));
  TEST_ASSERT_TRUE(a.start_sntp);
  // Accept a valid sample: Trusted, no further SNTP arm needed.
  a = step(&s, true, true, kRelease + 500, 20);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Trusted), ph(s.phase));
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_FALSE(a.start_sntp);
  TEST_ASSERT_EQUAL_INT64(kRelease + 500, s.trusted_unix);
}

void test_rejected_first_sample_goes_to_retry() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  step(&s, true, false, 0, 0);  // SyncPending
  step(&s, true, true, kRelease - kRollback - 1, 10);  // below floor: reject
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
}

void test_persisted_floor_raises_acceptance_floor() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease + 5000);  // floor = release + 5000
  step(&s, true, false, 0, 0);                  // SyncPending
  // A sample below (floor - rollback) is rejected.
  step(&s, true, true, kRelease + 5000 - kRollback - 1, 10);
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
  // Exactly (floor - rollback) is accepted (Retry also processes samples).
  step(&s, true, true, kRelease + 5000 - kRollback, 20);
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
}

// --- later rejected resync revokes trust ------------------------------------

void test_rejected_resync_revokes_trust() {
  TimeTrustState s = trustedState();
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  // A wildly-out-of-range resync (e.g. a rolled-back or hostile server) revokes.
  step(&s, true, true, kRelease - kRollback - 1, 5000);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
}

void test_accepted_resync_updates_trusted_time() {
  TimeTrustState s = trustedState();
  step(&s, true, true, kRelease + 2000, 6000);  // still in range
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_EQUAL_INT64(kRelease + 2000, s.trusted_unix);
}

// --- trusted time survives a brief link drop --------------------------------

void test_trusted_survives_brief_link_drop() {
  TimeTrustState s = trustedState();
  step(&s, false, false, 0, 1000);  // link drops: still Trusted
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Trusted), ph(s.phase));
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  step(&s, false, false, 0, 2000);  // still down: still Trusted
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  // Link returns with no fresh sample: trust persists, no forced re-sync.
  TimeTrustActions a = step(&s, true, false, 0, 3000);
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Trusted), ph(s.phase));
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_FALSE(a.start_sntp);
}

// --- retry/backoff transitions + millis rollover -----------------------------

void test_retry_backoff_doubles_to_cap() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  uint32_t now = 0;
  step(&s, true, false, 0, now);  // -> SyncPending, backoff=initial
  TEST_ASSERT_EQUAL_UINT32(kBackoffInit, s.retry_backoff_ms);

  const uint32_t waits[] = {500, 1000, 2000, 4000, 4000};
  for (uint32_t expected_wait : waits) {
    now += kSyncTimeout;
    step(&s, true, false, 0, now);  // SyncPending times out -> Retry
    TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
    now += expected_wait - 1;
    step(&s, true, false, 0, now);  // just before the wait: still Retry
    TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
    now += 1;
    TimeTrustActions a = step(&s, true, false, 0, now);  // wait elapsed
    TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::SyncPending), ph(s.phase));
    TEST_ASSERT_TRUE(a.start_sntp);
  }
  TEST_ASSERT_EQUAL_UINT32(kBackoffMax, s.retry_backoff_ms);  // capped
}

void test_retry_backoff_rollover_safe() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  s.phase = TimeTrustPhase::Retry;
  s.retry_backoff_ms = 500;
  s.phase_started_ms = 0xFFFFFF00u;  // near the uint32 rollover
  // Elapsed 499 ms across the wrap: still waiting.
  step(&s, true, false, 0, static_cast<uint32_t>(0xFFFFFF00u + 499u));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
  // Elapsed exactly 500 ms across the wrap: backoff satisfied.
  step(&s, true, false, 0, static_cast<uint32_t>(0xFFFFFF00u + 500u));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::SyncPending), ph(s.phase));
}

void test_sync_timeout_rollover_safe() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  s.phase = TimeTrustPhase::SyncPending;
  s.phase_started_ms = 0xFFFFFC00u;
  // Elapsed just under sync timeout across the wrap: still SyncPending.
  step(&s, true, false, 0, static_cast<uint32_t>(0xFFFFFC00u + kSyncTimeout - 1));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::SyncPending), ph(s.phase));
  step(&s, true, false, 0, static_cast<uint32_t>(0xFFFFFC00u + kSyncTimeout));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::Retry), ph(s.phase));
}

// --- fetch gate: blocked before trust, allowed immediately after ------------

void test_fetch_gate_blocks_until_trusted_then_allows() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease + 5000);
  // A due poll must NOT start while untrusted (latch stays pending).
  TEST_ASSERT_FALSE(core::adsbFetchAllowed(true, core::timeTrustIsTrusted(s)));
  step(&s, true, false, 0, 0);  // SyncPending, still untrusted
  TEST_ASSERT_FALSE(core::adsbFetchAllowed(true, core::timeTrustIsTrusted(s)));
  // Accept a valid sample (floor = release + 5000).
  step(&s, true, true, kRelease + 5000, 10);
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  // The same due poll is now allowed immediately.
  TEST_ASSERT_TRUE(core::adsbFetchAllowed(true, core::timeTrustIsTrusted(s)));
  // A not-due poll is never allowed, trusted or not.
  TEST_ASSERT_FALSE(core::adsbFetchAllowed(false, core::timeTrustIsTrusted(s)));
  TEST_ASSERT_FALSE(core::adsbFetchAllowed(false, false));
}

// --- derived trusted-now clock (monotonic anchor, not the wall clock) --------

void test_derived_now_untrusted_advances_and_goes_stale() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  int64_t out = -1;
  // Untrusted: no derived time, *out untouched.
  TEST_ASSERT_FALSE(core::derivedTrustedNowUnix(s, P, 0, &out));
  TEST_ASSERT_EQUAL_INT64(-1, out);

  step(&s, true, false, 0, 100);              // SyncPending
  step(&s, true, true, kRelease + 500, 200);  // Trusted, accepted_ms = 200
  // Elapsed 0 -> equals the accepted sample.
  TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(s, P, 200, &out));
  TEST_ASSERT_EQUAL_INT64(kRelease + 500, out);
  // 3000 ms later -> +3 s (integer seconds).
  TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(s, P, 200 + 3000, &out));
  TEST_ASSERT_EQUAL_INT64(kRelease + 503, out);
  // Just under the max age: still valid.
  TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(s, P, 200 + kMaxAge - 1, &out));
  // At/after the max age: stale -> false, *out untouched (fail closed).
  out = 777;
  TEST_ASSERT_FALSE(core::derivedTrustedNowUnix(s, P, 200 + kMaxAge, &out));
  TEST_ASSERT_EQUAL_INT64(777, out);
}

void test_derived_now_rollover_safe() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  s.phase = TimeTrustPhase::Trusted;
  s.trusted_unix = kRelease + 42;
  s.accepted_ms = 0xFFFFFF00u;  // near the uint32 rollover
  int64_t out = 0;
  // 2500 ms across the wrap -> +2 s, still fresh.
  TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(
      s, P, static_cast<uint32_t>(0xFFFFFF00u + 2500u), &out));
  TEST_ASSERT_EQUAL_INT64(kRelease + 44, out);
}

// --- stale accepted sample revokes trust and re-arms SNTP --------------------

void test_step_revokes_stale_trust_and_rearms() {
  TimeTrustState s = trustedState();  // accepted_ms = 10
  // A tick just before the max age keeps trust.
  step(&s, true, false, 0, 10 + kMaxAge - 1);
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  // A tick at the max age with no fresh sample revokes trust and re-arms SNTP.
  TimeTrustActions a = step(&s, true, false, 0, 10 + kMaxAge);
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::SyncPending), ph(s.phase));
  TEST_ASSERT_TRUE(a.start_sntp);
}

void test_step_stale_revoke_while_offline_fails_closed() {
  TimeTrustState s = trustedState();  // accepted_ms = 10
  // Stale AND offline: revoke to WaitingForWifi (fail closed, no link to re-arm).
  step(&s, false, false, 0, 10 + kMaxAge);
  TEST_ASSERT_FALSE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_EQUAL_INT(ph(TimeTrustPhase::WaitingForWifi), ph(s.phase));
}

void test_step_stale_tick_with_fresh_sample_reestablishes_trust() {
  TimeTrustState s = trustedState();  // accepted_ms = 10, trusted = kRelease+1000
  // On the very tick trust goes stale, a fresh valid sample re-anchors instantly.
  step(&s, true, true, kRelease + 3000, 10 + kMaxAge);
  TEST_ASSERT_TRUE(core::timeTrustIsTrusted(s));
  TEST_ASSERT_EQUAL_INT64(kRelease + 3000, s.trusted_unix);
  TEST_ASSERT_EQUAL_UINT32(10u + kMaxAge, s.accepted_ms);  // re-anchored
}

// --- versioned persisted-floor record (integrity + validation) ---------------

void test_persisted_record_roundtrip_and_rejection() {
  // The record is now a compact 16-byte {version, floor, checksum} blob.
  TEST_ASSERT_EQUAL_UINT32(16u, core::kPersistedFloorRecordBytes);

  uint8_t buf[core::kPersistedFloorRecordBytes];
  PersistedFloorRecord rec{kRelease + 500};
  TEST_ASSERT_TRUE(core::encodePersistedFloor(rec, buf, sizeof(buf)));

  PersistedFloorRecord out{};
  TEST_ASSERT_TRUE(
      core::decodeAndValidatePersistedFloor(P, buf, sizeof(buf), &out));
  TEST_ASSERT_EQUAL_INT64(kRelease + 500, out.floor_unix);

  // Wrong length (partial blob) -> invalid.
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, buf, sizeof(buf) - 1, &out));

  // A flipped floor byte breaks the checksum -> invalid (corrupt/torn write).
  uint8_t bad[core::kPersistedFloorRecordBytes];
  std::memcpy(bad, buf, sizeof(bad));
  bad[4] ^= 0xFF;  // a floor byte
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, bad, sizeof(bad), &out));

  // Wrong version -> invalid.
  std::memcpy(bad, buf, sizeof(bad));
  bad[0] ^= 0xFF;  // version byte
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, bad, sizeof(bad), &out));

  // Floor below release (stale/rollback) -> invalid, falls back to release.
  PersistedFloorRecord low{kRelease - 1};
  TEST_ASSERT_TRUE(core::encodePersistedFloor(low, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, buf, sizeof(buf), &out));

  // Floor above the future ceiling (future-invalid) -> invalid.
  PersistedFloorRecord hi{kRelease + kCeiling + 1};
  TEST_ASSERT_TRUE(core::encodePersistedFloor(hi, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, buf, sizeof(buf), &out));

  // A legacy 24-byte record (the previous {version, floor, last-write, checksum}
  // layout) is rejected on the exact-length check, so a firmware update falls
  // back safely to the release floor instead of misreading the old layout.
  uint8_t legacy[24] = {0};
  TEST_ASSERT_FALSE(
      core::decodeAndValidatePersistedFloor(P, legacy, sizeof(legacy), &out));
}

// --- CA-authenticated persisted-floor ratchet --------------------------------
// The ratchet candidate is a CA-signed peer leaf notBefore epoch (never an SNTP
// sample). The floor advances only when the authenticated candidate exceeds the
// stored floor by >= min_advance_s, and it is written EXACTLY (no cap, no SNTP).

void test_authenticated_notbefore_exact_advance_boundary() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);  // floor = kRelease
  uint64_t out = 777;
  // Just under the advance threshold (+min_advance - 1): not eligible.
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      s, kRelease + RP.min_advance_s - 1, 100, RP, &out));
  TEST_ASSERT_EQUAL_UINT64(777, out);  // untouched on rejection
  // Exactly floor + min_advance: eligible, writes the EXACT candidate (not `now`).
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(
      s, kRelease + RP.min_advance_s, 100, RP, &out));
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(kRelease + RP.min_advance_s),
                           out);
}

void test_malformed_zero_unverified_candidate_cannot_ratchet() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  uint64_t out = 777;  // sentinel; must stay untouched on every rejection
  // A zero candidate is exactly what a non-Ok / unverified fetch stamps.
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(s, 0, 100, RP, &out));
  // Below the release floor (stale / rollback / negative).
  TEST_ASSERT_FALSE(
      core::shouldRatchetPersistedFloor(s, kRelease - 1, 100, RP, &out));
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(s, -1, 100, RP, &out));
  // Above the finite future ceiling (corrupt / implausibly-future cert).
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      s, kRelease + RP.future_ceiling_s + 1, 100, RP, &out));
  TEST_ASSERT_EQUAL_UINT64(777, out);  // never written on any rejection
  TEST_ASSERT_EQUAL_INT64(kRelease, s.persisted_floor_unix);  // floor unmoved
}

void test_ratchet_failed_write_does_not_advance_but_guards() {
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  uint64_t out = 0;
  TEST_ASSERT_TRUE(
      core::shouldRatchetPersistedFloor(s, kRelease + 2000, 1000, RP, &out));
  // Simulate a FAILED NVS write: the floor is NOT advanced, but the in-session
  // flash-wear guard is still armed so a failure cannot storm the flash.
  core::noteFloorWrite(&s, out, 1000, /*success=*/false);
  TEST_ASSERT_EQUAL_INT64(kRelease, s.persisted_floor_unix);  // NOT advanced
  TEST_ASSERT_TRUE(s.floor_write_attempted);
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      s, kRelease + 2000, 1000 + RP.min_interval_ms - 1, RP, &out));
  // After the in-session interval it retries.
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(
      s, kRelease + 2000, 1000 + RP.min_interval_ms, RP, &out));
}

// A spoofed SNTP "now" that walks forward one step per reboot CANNOT walk the
// floor while the genuine endpoint keeps serving the SAME leaf notBefore.
void test_spoofed_sntp_across_reboots_cannot_walk_floor_same_cert() {
  const int64_t leaf_nb = kRelease + 3000;  // the fixed CA-signed notBefore

  // Boot 1: a fresh device. Even under a spoofed forward SNTP sample, the ratchet
  // writes EXACTLY the authenticated notBefore (advance 3000 >= min_advance).
  TimeTrustState s =
      rebootTrusted(kRelease, /*spoofed_now=*/kRelease + 8000, 100);
  uint64_t out = 0;
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(s, leaf_nb, 200, RP, &out));
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(leaf_nb), out);  // exact notBefore
  core::noteFloorWrite(&s, out, 200, /*success=*/true);
  TEST_ASSERT_EQUAL_INT64(leaf_nb, s.persisted_floor_unix);

  // Reboots 2..6: the SAME leaf notBefore is presented while the attacker walks
  // SNTP "now" forward. The floor stays pinned at leaf_nb every time.
  int64_t floor = s.persisted_floor_unix;
  for (int i = 1; i <= 5; ++i) {
    const int64_t spoofed_now = kRelease + 8000 + i * 500;  // attacker walks now
    TimeTrustState r = rebootTrusted(floor, spoofed_now, 100);
    TEST_ASSERT_EQUAL_INT64(leaf_nb, r.persisted_floor_unix);  // survived reboot
    // The derived (spoofed) trusted clock IS walking forward...
    int64_t derived = 0;
    TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(r, P, 101, &derived));
    TEST_ASSERT_EQUAL_INT64(spoofed_now, derived);
    // ...but the authenticated candidate has not changed, so it cannot advance,
    // now or later in the same boot.
    uint64_t o = 123;
    TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(r, leaf_nb, 200, RP, &o));
    TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
        r, leaf_nb, 200 + RP.min_interval_ms, RP, &o));
    TEST_ASSERT_EQUAL_UINT64(123, o);
    floor = r.persisted_floor_unix;
  }
  TEST_ASSERT_EQUAL_INT64(leaf_nb, floor);  // never walked past the one notBefore
}

// A same or older (concurrently-served alternate) cert notBefore neither rewrites
// nor rolls the floor back.
void test_same_or_older_cert_cannot_rewrite_or_rollback() {
  const int64_t leaf_nb = kRelease + 4000;
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  uint64_t out = 0;
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(s, leaf_nb, 100, RP, &out));
  core::noteFloorWrite(&s, out, 100, /*success=*/true);
  TEST_ASSERT_EQUAL_INT64(leaf_nb, s.persisted_floor_unix);

  // The SAME cert notBefore cannot rewrite (candidate == floor < floor+advance).
  uint64_t o = 0;
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      s, leaf_nb, 100 + RP.min_interval_ms, RP, &o));
  // An OLDER alternate cert (concurrently served / rotated back) cannot advance.
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      s, leaf_nb - 1500, 100 + RP.min_interval_ms, RP, &o));
  // And even a forced write of a lower value never rolls the floor back.
  core::noteFloorWrite(&s, static_cast<uint64_t>(leaf_nb - 1500),
                       100 + RP.min_interval_ms, /*success=*/true);
  TEST_ASSERT_EQUAL_INT64(leaf_nb, s.persisted_floor_unix);  // monotonic: no rollback
}

// A genuinely newer signed cert advances the floor exactly to ITS notBefore --
// never to the (spoofed) SNTP "now".
void test_newer_cert_advances_to_its_notbefore_not_sntp_now() {
  const int64_t nb0 = kRelease + 2000;  // initial genuine cert notBefore
  TimeTrustState s{};
  core::timeTrustInit(&s, P, kRelease);
  uint64_t out = 0;
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(s, nb0, 100, RP, &out));
  core::noteFloorWrite(&s, out, 100, /*success=*/true);
  TEST_ASSERT_EQUAL_INT64(nb0, s.persisted_floor_unix);

  // The endpoint rotates to a genuinely newer cert; drive trust under a spoofed
  // SNTP "now" far ahead of the new notBefore.
  const int64_t nb1 = nb0 + RP.min_advance_s + 500;  // newer, still under ceiling
  TimeTrustState r = rebootTrusted(s.persisted_floor_unix,
                                   /*spoofed_now=*/kRelease + 9000, 100);
  int64_t derived = 0;
  TEST_ASSERT_TRUE(core::derivedTrustedNowUnix(r, P, 101, &derived));
  TEST_ASSERT_EQUAL_INT64(kRelease + 9000, derived);  // SNTP now is far ahead
  uint64_t o = 0;
  TEST_ASSERT_TRUE(core::shouldRatchetPersistedFloor(r, nb1, 200, RP, &o));
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(nb1), o);  // exactly the notBefore
  TEST_ASSERT_TRUE(static_cast<int64_t>(o) < derived);      // NOT the SNTP now
  core::noteFloorWrite(&r, o, 200, /*success=*/true);
  TEST_ASSERT_EQUAL_INT64(nb1, r.persisted_floor_unix);  // advanced once

  // The same newer cert replayed cannot advance again.
  uint64_t again = 0;
  TEST_ASSERT_FALSE(core::shouldRatchetPersistedFloor(
      r, nb1, 200 + RP.min_interval_ms, RP, &again));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_effective_floor_selects_max_of_release_and_persisted);
  RUN_TEST(test_sample_classification_exact_boundaries);
  RUN_TEST(test_validate_persisted_floor_corrupt_older_future);
  RUN_TEST(test_boot_starts_untrusted_even_with_persisted_floor);
  RUN_TEST(test_wifi_and_sample_transitions);
  RUN_TEST(test_rejected_first_sample_goes_to_retry);
  RUN_TEST(test_persisted_floor_raises_acceptance_floor);
  RUN_TEST(test_rejected_resync_revokes_trust);
  RUN_TEST(test_accepted_resync_updates_trusted_time);
  RUN_TEST(test_trusted_survives_brief_link_drop);
  RUN_TEST(test_retry_backoff_doubles_to_cap);
  RUN_TEST(test_retry_backoff_rollover_safe);
  RUN_TEST(test_sync_timeout_rollover_safe);
  RUN_TEST(test_fetch_gate_blocks_until_trusted_then_allows);
  RUN_TEST(test_derived_now_untrusted_advances_and_goes_stale);
  RUN_TEST(test_derived_now_rollover_safe);
  RUN_TEST(test_step_revokes_stale_trust_and_rearms);
  RUN_TEST(test_step_stale_revoke_while_offline_fails_closed);
  RUN_TEST(test_step_stale_tick_with_fresh_sample_reestablishes_trust);
  RUN_TEST(test_persisted_record_roundtrip_and_rejection);
  RUN_TEST(test_authenticated_notbefore_exact_advance_boundary);
  RUN_TEST(test_malformed_zero_unverified_candidate_cannot_ratchet);
  RUN_TEST(test_ratchet_failed_write_does_not_advance_but_guards);
  RUN_TEST(test_spoofed_sntp_across_reboots_cannot_walk_floor_same_cert);
  RUN_TEST(test_same_or_older_cert_cannot_rewrite_or_rollback);
  RUN_TEST(test_newer_cert_advances_to_its_notbefore_not_sntp_now);
  return UNITY_END();
}
