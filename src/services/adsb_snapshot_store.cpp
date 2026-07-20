#include "services/adsb_snapshot_store.h"

namespace services::adsb {

SnapshotStore::SnapshotStore(uint64_t initial_generation)
    : snapshots_{},
      active_(0),
      generation_(initial_generation),
      candidate_slot_(0),
      candidate_revision_(0),
      candidate_valid_(false),
      candidate_outstanding_(false) {}

uint64_t SnapshotStore::nextGeneration(uint64_t generation) {
  const uint64_t advanced = generation + 1U;  // wrap is intentional and safe
  return advanced == kInvalidGeneration ? advanced + 1U : advanced;
}

bool SnapshotStore::tokenMatches(const CandidateHandle& handle) const {
  return handle.owner_ == this && handle.generation_ == generation_ &&
         handle.slot_ == candidate_slot_ &&
         handle.settings_revision_ == candidate_revision_ &&
         handle.valid_ == candidate_valid_;
}

CandidateHandle SnapshotStore::currentHandle(bool valid) const {
  CandidateHandle handle;
  handle.owner_ = this;
  handle.generation_ = generation_;
  handle.settings_revision_ = candidate_revision_;
  handle.slot_ = candidate_slot_;
  handle.valid_ = valid;
  return handle;
}

SnapshotView SnapshotStore::view() const {
  const AircraftSnapshot& s = snapshots_[active_];
  return SnapshotView{s.aircraft, s.count, s.settings_revision};
}

const Aircraft* SnapshotStore::aircraftList() const {
  return snapshots_[active_].aircraft;
}

size_t SnapshotStore::aircraftCount() const { return snapshots_[active_].count; }

uint32_t SnapshotStore::publishedRevision() const {
  return snapshots_[active_].settings_revision;
}

bool SnapshotStore::candidateOutstanding() const {
  return candidate_outstanding_;
}

CandidateResult SnapshotStore::fetchCandidate(const FetchRequest& request,
                                              FetchFn fetch, void* ctx) {
  // Begin a candidate: advance the generation (superseding any prior outstanding
  // candidate, since only one may be outstanding) and reserve the inactive slot.
  generation_ = nextGeneration(generation_);
  candidate_slot_ = inactiveIndex();
  candidate_revision_ = request.settings_revision;
  candidate_valid_ = false;
  candidate_outstanding_ = false;

  CandidateResult result{};

  if (fetch == nullptr) {
    result.fetch.outcome = FetchOutcome::ParseError;
    result.fetch.http_status = -1;
    result.handle = currentHandle(false);
    return result;
  }

  result.fetch = fetch(request, snapshots_[candidate_slot_], ctx);

  if (result.fetch.outcome == FetchOutcome::Ok) {
    // Stamp the published revision from the request so the active view's
    // settings_revision is authoritative independent of the fetcher.
    snapshots_[candidate_slot_].settings_revision = request.settings_revision;
    candidate_valid_ = true;
    candidate_outstanding_ = true;
  }
  // A non-Ok result leaves candidate_outstanding_ false: the candidate is
  // auto-discarded and the active snapshot is untouched.
  result.handle = currentHandle(candidate_valid_);
  return result;
}

PublishResult SnapshotStore::publishCandidate(
    const CandidateHandle& handle, uint32_t current_settings_revision) {
  if (!tokenMatches(handle)) {
    return PublishResult::InvalidHandle;  // foreign, stale, forged, or default
  }
  if (!candidate_outstanding_) {
    return PublishResult::NoCandidate;  // failed fetch, or already resolved
  }
  if (candidate_revision_ != current_settings_revision) {
    candidate_outstanding_ = false;  // obsolete candidate is resolved (rejected)
    return PublishResult::ObsoleteRevision;  // active preserved byte-for-byte
  }
  active_ = candidate_slot_;  // single index switch; no snapshot copy
  candidate_outstanding_ = false;
  return PublishResult::Published;
}

void SnapshotStore::discardCandidate(const CandidateHandle& handle) {
  if (tokenMatches(handle) && candidate_outstanding_) {
    candidate_outstanding_ = false;
  }
}

}  // namespace services::adsb
