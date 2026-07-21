#include "services/timekeeper.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <sys/time.h>  // struct timeval (SNTP callback argument)

#include <cstdint>

#include "config.h"
#include "core/time_trust.h"
#include "runtime_diagnostics.h"

// Deliberately NO <ctime> / ::time(nullptr): nowUnix and the certificate
// notBefore/notAfter check read only the core's DERIVED accepted-monotonic clock
// (core::derivedTrustedNowUnix). The persisted-floor ratchet reads NO clock at
// all -- its candidate is the CA-signed peer leaf notBefore supplied by the
// caller, never SNTP-derived time -- so an unauthenticated NTP attacker cannot
// poison NVS. lwIP applies settimeofday to the system wall clock BEFORE this
// file's sync callback runs -- even for a sample the core later rejects -- so the
// mutable wall clock must never gate trusted state. scripts/verify-ca-bundle.ps1
// fails the build if a raw time(nullptr)/time(NULL) call appears in this file, or
// if the floor ratchet reads the derived/SNTP clock.

namespace services::timekeeper {

namespace {

// Canonical acceptance policy, built once from config.h (mirrors
// core::TimeTrustPolicy).
constexpr core::TimeTrustPolicy kPolicy = {
    config::kReleaseEpochFloorUnix,
    config::kTimeRollbackToleranceSec,
    config::kTimeFutureCeilingSec,
    config::kTimeSyncTimeoutMs,
    config::kTimeRetryBackoffInitialMs,
    config::kTimeRetryBackoffMaxMs,
    config::kTimeTrustedSampleMaxAgeMs,
};

// CA-authenticated persisted-floor ratchet policy (plausible-epoch window + 24 h
// authenticated-advance throttle + in-session flash guard). The candidate is the
// CA-signed peer leaf notBefore, so no SNTP/derived clock is read here.
constexpr core::FloorRatchetPolicy kRatchetPolicy = {
    config::kReleaseEpochFloorUnix,
    config::kTimeFutureCeilingSec,
    config::kTimeFloorMinAdvanceSec,
    config::kTimeFloorPersistIntervalMs,
};

core::TimeTrustState s_state{};
uint32_t s_last_consumed_gen = 0;
bool s_floor_write_warned = false;

// Dedicated spinlock guarding s_state so the future worker-on build can read
// trusted()/nowUnix() from the worker task while the loop task runs update() and
// noteVerifiedCertFloor() without a torn 64-bit field read or a mid-step race. It
// is SEPARATE from s_sample_mux (which only guards the SNTP sample latch). Every
// critical section stays short: copy or step s_state under the lock, then perform
// SNTP start, NVS I/O, and logging OUTSIDE it -- the mux is never held during
// NVS/network/logging.
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

// --- Thread-safe SNTP sample latch (portMUX critical section) ----------------
// The SNTP notification callback runs in the lwIP task and may preempt the loop
// task. It only latches the sample: no logging, allocation, NVS, drawing, or
// networking. The 64-bit epoch and its generation counter are copied under the
// repository's established portMUX_TYPE spinlock (see s_boot_mux in
// wifi_setup.cpp), which serializes writer and reader on every core with full
// ordering -- no single-core-only weak-ordering assumption and no torn 64-bit
// read.
portMUX_TYPE s_sample_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_sample_unix = 0;
uint32_t s_sample_gen = 0;  // 0 => no sample latched yet; else increments per sample

void sntpSyncCallback(struct timeval* tv) {
  const int64_t sec = (tv != nullptr) ? static_cast<int64_t>(tv->tv_sec) : 0;
  portENTER_CRITICAL(&s_sample_mux);
  s_sample_unix = sec;
  ++s_sample_gen;
  portEXIT_CRITICAL(&s_sample_mux);
}

// Read the latched sample if one has ever been published. Returns false before
// the first sample. *out_gen is the sample generation, used to detect a fresh
// sample vs the last consumed one.
bool readLatchedSample(int64_t* out_sec, uint32_t* out_gen) {
  portENTER_CRITICAL(&s_sample_mux);
  const uint32_t gen = s_sample_gen;
  const int64_t sec = s_sample_unix;
  portEXIT_CRITICAL(&s_sample_mux);
  if (gen == 0u) {
    return false;  // no sample latched yet
  }
  *out_gen = gen;
  *out_sec = sec;
  return true;
}

// Arm (or re-arm) SNTP via the pinned non-blocking configTime path. DHCP NTP
// stays disabled. Empty fallback server constants become nullptr (unused slots).
void startSntp() {
#if defined(LWIP_DHCP_GET_NTP_SRV) && LWIP_DHCP_GET_NTP_SRV
  esp_sntp_servermode_dhcp(false);  // never accept DHCP-provided NTP servers
#endif
  const char* s1 = config::kSntpServerPrimary;
  const char* s2 =
      config::kSntpServerFallback1[0] != '\0' ? config::kSntpServerFallback1 : nullptr;
  const char* s3 =
      config::kSntpServerFallback2[0] != '\0' ? config::kSntpServerFallback2 : nullptr;
  configTime(0, 0, s1, s2, s3);  // UTC (no offset); non-blocking SNTP start
}

// Read + validate the versioned persisted-floor record. Corrupt/partial/out-of-
// range blobs are ignored: the release floor stands and no elevated floor is
// adopted, so trust starts from the safe baseline.
void readPersistedRecord(int64_t* out_floor) {
  *out_floor = config::kReleaseEpochFloorUnix;
  Preferences prefs;
  if (prefs.begin(config::kTimeFloorNvsNamespace, /*readOnly=*/true)) {
    if (prefs.isKey(config::kTimeFloorNvsKey) &&
        prefs.getBytesLength(config::kTimeFloorNvsKey) ==
            core::kPersistedFloorRecordBytes) {
      uint8_t buf[core::kPersistedFloorRecordBytes];
      const size_t n =
          prefs.getBytes(config::kTimeFloorNvsKey, buf, sizeof(buf));
      core::PersistedFloorRecord rec{};
      if (n == core::kPersistedFloorRecordBytes &&
          core::decodeAndValidatePersistedFloor(
              kPolicy, buf, static_cast<uint32_t>(n), &rec)) {
        *out_floor = rec.floor_unix;
      }
      // A corrupt / partial / out-of-range blob is ignored: the release floor
      // stands (never weakened) and no elevated floor is claimed.
    }
    prefs.end();
  }
}

}  // namespace

void init() {
  int64_t persisted_floor = config::kReleaseEpochFloorUnix;
  readPersistedRecord(&persisted_floor);  // NVS read OUTSIDE the lock
  portENTER_CRITICAL(&s_state_mux);
  core::timeTrustInit(&s_state, kPolicy, persisted_floor);  // always UNTRUSTED at boot
  portEXIT_CRITICAL(&s_state_mux);
  s_last_consumed_gen = 0;
  sntp_set_time_sync_notification_cb(sntpSyncCallback);
}

void update(bool wifi_connected, uint32_t now_ms) {
  core::TimeTrustInputs in{};
  in.wifi_up = wifi_connected;
  in.now_ms = now_ms;

  int64_t sample_unix = 0;
  uint32_t gen = 0;
  if (readLatchedSample(&sample_unix, &gen) && gen != s_last_consumed_gen) {
    s_last_consumed_gen = gen;
    in.sample_available = true;
    in.sample_unix = sample_unix;
  }

  // Step the state machine under the lock, then perform any SNTP (re)arm OUTSIDE
  // it -- the mux is never held during network/logging.
  core::TimeTrustActions actions;
  portENTER_CRITICAL(&s_state_mux);
  actions = core::timeTrustStep(&s_state, kPolicy, in);
  portEXIT_CRITICAL(&s_state_mux);
  if (actions.start_sntp) {
    startSntp();
  }
}

bool trusted() {
  portENTER_CRITICAL(&s_state_mux);
  const bool is_trusted = core::timeTrustIsTrusted(s_state);
  portEXIT_CRITICAL(&s_state_mux);
  return is_trusted;
}

int64_t nowUnix() {
  const uint32_t now_ms = millis();
  // Snapshot s_state under the lock, then compute the derived value outside it.
  core::TimeTrustState snapshot;
  portENTER_CRITICAL(&s_state_mux);
  snapshot = s_state;
  portEXIT_CRITICAL(&s_state_mux);
  int64_t derived = 0;
  if (!core::derivedTrustedNowUnix(snapshot, kPolicy, now_ms, &derived)) {
    return 0;  // untrusted or stale: fail closed (never the mutable wall clock)
  }
  return derived;
}

void noteVerifiedCertFloor(int64_t authenticated_cert_not_before_unix,
                           uint32_t now_ms) {
  // The persisted-floor candidate is the CA-signed peer leaf notBefore supplied
  // by the caller after a COMPLETE verified response -- NEVER an SNTP-derived or
  // wall-clock value. This path reads no trusted/derived clock, so an
  // unauthenticated NTP attacker cannot influence what is written to NVS. A
  // non-Ok fetch passes 0, which the core rejects as below the release floor.
  uint64_t floor_value = 0;
  bool should_write = false;
  portENTER_CRITICAL(&s_state_mux);
  should_write = core::shouldRatchetPersistedFloor(
      s_state, authenticated_cert_not_before_unix, now_ms, kRatchetPolicy,
      &floor_value);
  portEXIT_CRITICAL(&s_state_mux);
  if (!should_write) {
    return;
  }

  // Serialize the versioned record {floor = exact authenticated notBefore} and
  // write it as a single atomic NVS blob. The mux is NOT held across this I/O.
  const core::PersistedFloorRecord rec{static_cast<int64_t>(floor_value)};
  uint8_t buf[core::kPersistedFloorRecordBytes];
  bool ok = false;
  if (core::encodePersistedFloor(rec, buf, sizeof(buf))) {
    Preferences prefs;
    if (prefs.begin(config::kTimeFloorNvsNamespace, /*readOnly=*/false)) {
      ok = prefs.putBytes(config::kTimeFloorNvsKey, buf, sizeof(buf)) ==
           sizeof(buf);
      prefs.end();
    }
  }

  // Record the attempt (success or failure) so the in-session flash-wear guard is
  // honored consistently; only a successful write advances the in-RAM floor
  // (monotonic max, never a rollback). Take the lock again ONLY to record it.
  portENTER_CRITICAL(&s_state_mux);
  core::noteFloorWrite(&s_state, floor_value, now_ms, ok);
  portEXIT_CRITICAL(&s_state_mux);
  if (!ok && !s_floor_write_warned) {
    s_floor_write_warned = true;  // surface once; do not spam, do not block
    PLANE_RADAR_LOG_E("time: persisted-floor NVS write failed (trust unaffected)\n");
  }
}

bool clearPersistedFloor() {
  Preferences prefs;
  if (!prefs.begin(config::kTimeFloorNvsNamespace, /*readOnly=*/false)) {
    // The namespace may simply not exist yet (nothing ever written). Treat an
    // open failure conservatively as a clear failure so the reset path never
    // silently claims success while a poisoned record could survive.
    return false;
  }
  const bool ok = prefs.clear();  // wipe every key in the dedicated namespace
  prefs.end();
  return ok;
}

}  // namespace services::timekeeper
