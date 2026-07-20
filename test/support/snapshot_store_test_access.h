#pragma once

// Test-only seam for adsb_snapshot_store. CandidateHandle deliberately keeps its
// identity fields private so production code cannot forge or mutate a token. To
// still exercise the store's full-token validation (e.g. a token bound to the
// wrong slot or revision), tests derive variant handles through this sanctioned
// friend accessor -- never by reaching into public fields, which do not exist.
// It is declared a friend in adsb_snapshot_store.h only when
// PLANE_RADAR_NATIVE_TEST_ACCESS is defined, which happens only in the
// [env:native] build_flags in platformio.ini. Firmware environments (e.g.
// [env:supermini]) never define that macro, so SnapshotStoreTestAccess is not a
// friend there and this header intentionally fails to compile if it is ever
// pulled into a non-native build -- there is no test-only access surface for
// production code to (ab)use.

#include <cstdint>

#include "services/adsb_snapshot_store.h"

#ifndef PLANE_RADAR_NATIVE_TEST_ACCESS
#error "snapshot_store_test_access.h is native-test-only; it requires " \
       "PLANE_RADAR_NATIVE_TEST_ACCESS (set only in [env:native])"
#endif

namespace services::adsb {

struct SnapshotStoreTestAccess {
  // Read the bound identity of an issued handle.
  static const SnapshotStore* owner(const CandidateHandle& h) { return h.owner_; }
  static uint64_t generation(const CandidateHandle& h) { return h.generation_; }
  static uint32_t revision(const CandidateHandle& h) {
    return h.settings_revision_;
  }
  static uint8_t slot(const CandidateHandle& h) { return h.slot_; }
  static bool valid(const CandidateHandle& h) { return h.valid_; }

  // Derive a variant handle with a single identity field replaced, keeping every
  // other field intact. Used to construct legitimate-but-wrong tokens (wrong
  // slot, wrong revision, wrong generation, wrong owner) for validation tests.
  static CandidateHandle withSlot(CandidateHandle h, uint8_t slot) {
    h.slot_ = slot;
    return h;
  }
  static CandidateHandle withRevision(CandidateHandle h, uint32_t revision) {
    h.settings_revision_ = revision;
    return h;
  }
  static CandidateHandle withGeneration(CandidateHandle h, uint64_t generation) {
    h.generation_ = generation;
    return h;
  }
  static CandidateHandle withOwner(CandidateHandle h, const SnapshotStore* owner) {
    h.owner_ = owner;
    return h;
  }
};

}  // namespace services::adsb
