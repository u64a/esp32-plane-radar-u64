#pragma once

// Arduino-free two-step candidate publication for the nearest-aircraft snapshot.
// Owns exactly two AircraftSnapshots and publishes with a single index switch:
// no snapshot copies, no third buffer. A pre-publication revision gate lets a
// slow fetch that was started for one settings revision be discarded instead of
// published against a newer revision, all without touching Arduino or TLS code.

#include <cstddef>
#include <cstdint>

#include "services/adsb_types.h"

namespace services::adsb {

class SnapshotStore;
#ifdef PLANE_RADAR_NATIVE_TEST_ACCESS
struct SnapshotStoreTestAccess;
#endif

// Compact, copy-free view of the published snapshot. The pointer, count, and
// revision are captured together so they are always mutually consistent.
struct SnapshotView {
  const Aircraft* aircraft;
  uint16_t count;
  uint32_t settings_revision;
};

// One fetch request: the query parameters plus the settings revision the query
// belongs to. The store carries the revision through to publication.
struct FetchRequest {
  double lat;
  double lon;
  float radius_km;
  uint32_t settings_revision;
};

// Opaque token for one outstanding candidate. Construct one only via
// SnapshotStore::fetchCandidate; a default-constructed handle is permanently
// invalid. Internally it binds the *full* identity of the candidate that issued
// it -- the owning SnapshotStore, the target buffer slot, the settings revision,
// a 64-bit nonzero generation, and the fetch-success flag -- so publish/discard
// can reject a handle from another store, a zero-initialized handle (even after
// the generation wraps), and any stale or superseded handle. Fields are private
// so callers cannot forge or mutate a token; only the issuing store fills them.
// The token is a few machine words and never embeds or copies a snapshot, so a
// later phase can hold a small queue of them cheaply. The SnapshotStoreTestAccess
// friend below only exists when PLANE_RADAR_NATIVE_TEST_ACCESS is defined (the
// [env:native] build only); firmware builds (e.g. [env:supermini]) never define
// that macro, so no production translation unit can declare
// SnapshotStoreTestAccess and gain friend access to these private fields.
class CandidateHandle {
 public:
  CandidateHandle() = default;

  // True only for a handle whose fetch succeeded (a publish was possible when it
  // was issued). A default/never-issued handle is always false.
  bool valid() const { return valid_; }

  // Echoes the settings revision the candidate was fetched for.
  uint32_t settingsRevision() const { return settings_revision_; }

 private:
  friend class SnapshotStore;
#ifdef PLANE_RADAR_NATIVE_TEST_ACCESS
  friend struct SnapshotStoreTestAccess;
#endif

  const SnapshotStore* owner_ = nullptr;  // issuing store; nullptr == invalid
  uint64_t generation_ = 0;               // 0 is the reserved invalid sentinel
  uint32_t settings_revision_ = 0;
  uint8_t slot_ = 0;                       // target (inactive) buffer slot, 0/1
  bool valid_ = false;                     // fetch succeeded, publish possible
};

// FetchResult plus the handle for the candidate that fetchCandidate() produced.
struct CandidateResult {
  FetchResult fetch;
  CandidateHandle handle;
};

// Explicit outcome of publishCandidate(). Only Published switches the active
// snapshot; every other value preserves the active snapshot byte-for-byte.
enum class PublishResult : uint8_t {
  Published,         // token current, fetch Ok, revisions match: active switched
  ObsoleteRevision,  // candidate revision != current settings revision
  InvalidHandle,     // token not the current candidate (foreign/stale/forged/default)
  NoCandidate,       // token is the current candidate but none is outstanding
};

// Fetch seam: fills `out` for `request` and returns the FetchResult. Arduino-free
// so the firmware injects the real WiFiClientSecure fetch and tests inject a
// fake. `ctx` is an opaque caller cookie (may be null).
using FetchFn = FetchResult (*)(const FetchRequest& request, AircraftSnapshot& out,
                                void* ctx);

class SnapshotStore {
 public:
  // initial_generation seeds the candidate id sequence (test seam for exercising
  // generation wrap); production uses the default. The first issued generation is
  // always nonzero: the sequence advances before use and skips the 0 sentinel on
  // wrap, so a zero-initialized handle never matches an issued candidate.
  explicit SnapshotStore(uint64_t initial_generation = 0);

  // The store hands out handles that point back to it, so it must never be copied
  // or moved out from under an outstanding token.
  SnapshotStore(const SnapshotStore&) = delete;
  SnapshotStore& operator=(const SnapshotStore&) = delete;

  // Published (active) snapshot, read by the renderer.
  SnapshotView view() const;
  const Aircraft* aircraftList() const;
  size_t aircraftCount() const;
  uint32_t publishedRevision() const;

  // Step 1: begin a candidate in the inactive slot and fill it via `fetch`.
  // Beginning a candidate supersedes any prior outstanding one (only one may be
  // outstanding). A non-Ok result auto-discards the candidate (handle.valid() is
  // false) and leaves the active snapshot untouched.
  CandidateResult fetchCandidate(const FetchRequest& request, FetchFn fetch,
                                 void* ctx);

  // Step 2: publish iff the handle is the current candidate token, its fetch
  // succeeded, and its revision equals current_settings_revision. Otherwise the
  // active snapshot is preserved byte-for-byte and the reason is returned.
  PublishResult publishCandidate(const CandidateHandle& handle,
                                 uint32_t current_settings_revision);

  // Discard an outstanding candidate. Safe and idempotent: a token that is not
  // the current outstanding candidate (stale, foreign, forged, or already
  // resolved) is a no-op.
  void discardCandidate(const CandidateHandle& handle);

  bool candidateOutstanding() const;

 private:
#ifdef PLANE_RADAR_NATIVE_TEST_ACCESS
  friend struct SnapshotStoreTestAccess;
#endif

  static constexpr uint64_t kInvalidGeneration = 0;

  uint8_t inactiveIndex() const { return static_cast<uint8_t>(active_ ^ 1U); }

  // Advance the generation sequence, skipping the reserved invalid sentinel so a
  // wrapped generation is always nonzero.
  static uint64_t nextGeneration(uint64_t generation);

  // Full-token identity check: the handle must have been issued by this store for
  // the exact candidate that is current (same generation, slot, revision, and
  // success flag). Generation alone would already reject the common stale case;
  // the remaining fields close cross-store and forged-token gaps.
  bool tokenMatches(const CandidateHandle& handle) const;

  // Build the handle for the candidate just issued.
  CandidateHandle currentHandle(bool valid) const;

  AircraftSnapshot snapshots_[2];
  uint8_t active_;
  uint64_t generation_;          // last issued candidate generation (nonzero)
  uint8_t candidate_slot_;       // slot the last-issued candidate targets
  uint32_t candidate_revision_;  // revision the last-issued candidate was fetched for
  bool candidate_valid_;         // last-issued candidate's fetch succeeded
  bool candidate_outstanding_;   // a publishable candidate awaits resolution
};

}  // namespace services::adsb
