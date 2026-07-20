#include <unity.h>

#include <cstdint>

#include "services/adsb_snapshot_store.h"

#include "../support/snapshot_store_test_access.h"

using namespace services::adsb;
using Access = services::adsb::SnapshotStoreTestAccess;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// Scripted fetch seam: writes a recognizable marker into the inactive snapshot on
// success and a poison value on failure, so tests can prove which slot is active
// and that a failed fetch never leaks into the published view.
struct FakeFetch {
  FetchOutcome outcome = FetchOutcome::Ok;
  uint16_t count = 0;
  float marker = 0.0f;
  int calls = 0;
};

FetchResult fakeFetch(const FetchRequest& request, AircraftSnapshot& out,
                      void* ctx) {
  FakeFetch* f = static_cast<FakeFetch*>(ctx);
  f->calls += 1;
  FetchResult r{};
  r.outcome = f->outcome;
  if (f->outcome == FetchOutcome::Ok) {
    r.http_status = 200;
    r.aircraft_count = f->count;
    out.count = f->count;
    out.settings_revision = request.settings_revision;
    out.aircraft[0].lat = f->marker;
  } else {
    // Simulate a partially-written inactive slot to prove active preservation.
    r.http_status = -1;
    out.count = 999;
    out.aircraft[0].lat = -1.0f;
  }
  return r;
}

FetchRequest req(uint32_t revision, float marker_query = 0.0f) {
  return FetchRequest{marker_query, marker_query, 10.0f, revision};
}

int pub(PublishResult r) { return static_cast<int>(r); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_initial_view_is_empty_at_revision_zero() {
  SnapshotStore store;
  const SnapshotView v = store.view();
  TEST_ASSERT_EQUAL_UINT16(0, v.count);
  TEST_ASSERT_EQUAL_UINT32(0, v.settings_revision);
  TEST_ASSERT_EQUAL_size_t(0, store.aircraftCount());
  TEST_ASSERT_NOT_NULL(store.aircraftList());
  TEST_ASSERT_FALSE(store.candidateOutstanding());
}

void test_successful_candidate_publishes_and_switches_active() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 3, 12.5f, 0};
  CandidateResult cr = store.fetchCandidate(req(7), &fakeFetch, &f);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok),
                        static_cast<int>(cr.fetch.outcome));
  TEST_ASSERT_TRUE(cr.handle.valid());
  TEST_ASSERT_EQUAL_UINT32(7, cr.handle.settingsRevision());
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  // Active view is unchanged until publish.
  TEST_ASSERT_EQUAL_UINT16(0, store.view().count);

  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(cr.handle, 7)));
  TEST_ASSERT_FALSE(store.candidateOutstanding());
  const SnapshotView v = store.view();
  TEST_ASSERT_EQUAL_UINT16(3, v.count);
  TEST_ASSERT_EQUAL_UINT32(7, v.settings_revision);
  TEST_ASSERT_EQUAL_FLOAT(12.5f, v.aircraft[0].lat);
}

void test_empty_success_publishes() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 0, 0.0f, 0};
  CandidateResult cr = store.fetchCandidate(req(2), &fakeFetch, &f);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(cr.handle, 2)));
  TEST_ASSERT_EQUAL_UINT16(0, store.view().count);
  TEST_ASSERT_EQUAL_UINT32(2, store.view().settings_revision);
}

void test_failed_fetch_auto_discards_and_preserves_active() {
  SnapshotStore store;
  FakeFetch good{FetchOutcome::Ok, 2, 7.0f, 0};
  CandidateResult c1 = store.fetchCandidate(req(1), &fakeFetch, &good);
  store.publishCandidate(c1.handle, 1);
  const SnapshotView before = store.view();

  FakeFetch bad{FetchOutcome::Timeout, 0, 0.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(1), &fakeFetch, &bad);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Timeout),
                        static_cast<int>(c2.fetch.outcome));
  TEST_ASSERT_FALSE(c2.handle.valid());
  TEST_ASSERT_FALSE(store.candidateOutstanding());

  const SnapshotView after = store.view();
  TEST_ASSERT_EQUAL_PTR(before.aircraft, after.aircraft);  // same active slot
  TEST_ASSERT_EQUAL_UINT16(2, after.count);
  TEST_ASSERT_EQUAL_UINT32(1, after.settings_revision);
  TEST_ASSERT_EQUAL_FLOAT(7.0f, after.aircraft[0].lat);
  // Publishing the failed handle is a no-op with an explicit reason: the token
  // is the current candidate (matches identity) but nothing is outstanding.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::NoCandidate),
                        pub(store.publishCandidate(c2.handle, 1)));
  TEST_ASSERT_EQUAL_UINT16(2, store.view().count);
}

void test_obsolete_revision_preserves_active_and_resolves_candidate() {
  SnapshotStore store;
  FakeFetch f1{FetchOutcome::Ok, 2, 7.0f, 0};
  CandidateResult c1 = store.fetchCandidate(req(1), &fakeFetch, &f1);
  store.publishCandidate(c1.handle, 1);

  // A slow candidate fetched for revision 2, but settings advanced to 3.
  FakeFetch f2{FetchOutcome::Ok, 5, 9.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(2), &fakeFetch, &f2);
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::ObsoleteRevision),
                        pub(store.publishCandidate(c2.handle, 3)));
  TEST_ASSERT_FALSE(store.candidateOutstanding());
  // Active still holds the revision-1 snapshot byte-for-byte.
  TEST_ASSERT_EQUAL_UINT16(2, store.view().count);
  TEST_ASSERT_EQUAL_UINT32(1, store.view().settings_revision);
  TEST_ASSERT_EQUAL_FLOAT(7.0f, store.view().aircraft[0].lat);
  // Re-publishing the resolved handle is a no-op.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::NoCandidate),
                        pub(store.publishCandidate(c2.handle, 2)));
}

void test_stale_handle_after_supersession_is_invalid() {
  SnapshotStore store;
  FakeFetch f1{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult c1 = store.fetchCandidate(req(1), &fakeFetch, &f1);
  // Beginning a second candidate supersedes the first (only one outstanding).
  FakeFetch f2{FetchOutcome::Ok, 2, 2.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(1), &fakeFetch, &f2);
  TEST_ASSERT_TRUE(Access::generation(c1.handle) != Access::generation(c2.handle));
  TEST_ASSERT_TRUE(store.candidateOutstanding());  // exactly one

  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(c1.handle, 1)));
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c2.handle, 1)));
  TEST_ASSERT_EQUAL_UINT16(2, store.view().count);
}

void test_discard_is_idempotent_and_scoped_to_the_handle() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 4, 3.0f, 0};
  CandidateResult c = store.fetchCandidate(req(1), &fakeFetch, &f);
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  store.discardCandidate(c.handle);
  TEST_ASSERT_FALSE(store.candidateOutstanding());
  store.discardCandidate(c.handle);  // idempotent
  TEST_ASSERT_FALSE(store.candidateOutstanding());
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::NoCandidate),
                        pub(store.publishCandidate(c.handle, 1)));
  TEST_ASSERT_EQUAL_UINT16(0, store.view().count);

  // A stale handle must not discard a newer outstanding candidate.
  FakeFetch a{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult ca = store.fetchCandidate(req(1), &fakeFetch, &a);
  FakeFetch b{FetchOutcome::Ok, 2, 2.0f, 0};
  CandidateResult cb = store.fetchCandidate(req(1), &fakeFetch, &b);
  store.discardCandidate(ca.handle);  // stale -> no-op
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(cb.handle, 1)));
}

void test_active_view_pointer_switches_between_two_slots() {
  SnapshotStore store;
  const Aircraft* slot_a = store.aircraftList();
  FakeFetch f1{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult c1 = store.fetchCandidate(req(1), &fakeFetch, &f1);
  TEST_ASSERT_EQUAL_PTR(slot_a, store.aircraftList());  // unchanged while outstanding
  store.publishCandidate(c1.handle, 1);
  const Aircraft* slot_b = store.aircraftList();
  TEST_ASSERT_TRUE(slot_a != slot_b);  // switched slots, no copy

  FakeFetch f2{FetchOutcome::Ok, 2, 2.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(2), &fakeFetch, &f2);
  store.publishCandidate(c2.handle, 2);
  TEST_ASSERT_EQUAL_PTR(slot_a, store.aircraftList());  // back to the first slot
}

void test_revision_wrap_uses_exact_equality() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult c = store.fetchCandidate(req(UINT32_MAX), &fakeFetch, &f);
  // current revision wrapped to 0: candidate at UINT32_MAX is obsolete.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::ObsoleteRevision),
                        pub(store.publishCandidate(c.handle, 0)));
  // A matching revision at the wrap boundary publishes exactly.
  FakeFetch f2{FetchOutcome::Ok, 1, 2.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(UINT32_MAX), &fakeFetch, &f2);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c2.handle, UINT32_MAX)));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, store.view().settings_revision);
}

// --- Blocker-1 regression coverage: full-token identity validation. ----------

void test_default_handle_is_rejected_and_preserves_active() {
  SnapshotStore store;
  FakeFetch f0{FetchOutcome::Ok, 4, 4.0f, 0};
  CandidateResult seed = store.fetchCandidate(req(0), &fakeFetch, &f0);
  store.publishCandidate(seed.handle, 0);  // active now holds 4 aircraft

  // A real candidate is outstanding; a default/never-issued handle must not
  // publish it (its owner is null, its generation is the reserved 0 sentinel).
  FakeFetch f{FetchOutcome::Ok, 9, 9.0f, 0};
  CandidateResult c = store.fetchCandidate(req(0), &fakeFetch, &f);
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  const CandidateHandle default_handle{};
  TEST_ASSERT_FALSE(default_handle.valid());
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(default_handle, 0)));
  store.discardCandidate(default_handle);  // also a no-op
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  TEST_ASSERT_EQUAL_UINT16(4, store.view().count);  // active preserved
  // The genuine handle still resolves the candidate.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c.handle, 0)));
  TEST_ASSERT_EQUAL_UINT16(9, store.view().count);
}

void test_cross_store_handle_is_rejected() {
  SnapshotStore a;
  SnapshotStore b;
  // Both stores independently reach generation 1 for revision 0, so a bare
  // generation check would confuse them; the owner binding must not.
  FakeFetch fa{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult ca = a.fetchCandidate(req(0), &fakeFetch, &fa);
  FakeFetch fb{FetchOutcome::Ok, 2, 2.0f, 0};
  CandidateResult cb = b.fetchCandidate(req(0), &fakeFetch, &fb);
  TEST_ASSERT_TRUE(Access::generation(ca.handle) == Access::generation(cb.handle));

  // Store A's handle must be rejected by store B and leave B outstanding.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(b.publishCandidate(ca.handle, 0)));
  b.discardCandidate(ca.handle);  // foreign discard is also a no-op
  TEST_ASSERT_TRUE(b.candidateOutstanding());
  // Each store still resolves its own handle.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(b.publishCandidate(cb.handle, 0)));
  TEST_ASSERT_EQUAL_UINT16(2, b.view().count);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(a.publishCandidate(ca.handle, 0)));
  TEST_ASSERT_EQUAL_UINT16(1, a.view().count);
}

void test_duplicate_publish_is_a_no_op() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 5, 5.0f, 0};
  CandidateResult c = store.fetchCandidate(req(3), &fakeFetch, &f);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c.handle, 3)));
  // Replaying the exact same successful handle must not republish.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::NoCandidate),
                        pub(store.publishCandidate(c.handle, 3)));
  store.discardCandidate(c.handle);  // discard of a resolved token: no-op
  TEST_ASSERT_FALSE(store.candidateOutstanding());
  TEST_ASSERT_EQUAL_UINT16(5, store.view().count);
}

void test_wrong_slot_token_is_rejected() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 6, 6.0f, 0};
  CandidateResult c = store.fetchCandidate(req(4), &fakeFetch, &f);
  const uint8_t good_slot = Access::slot(c.handle);
  // A token identical to the outstanding candidate except for its bound slot
  // (a stand-in for a future queue's slot mix-up) must not validate.
  const CandidateHandle wrong_slot =
      Access::withSlot(c.handle, static_cast<uint8_t>(good_slot ^ 1U));
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(wrong_slot, 4)));
  store.discardCandidate(wrong_slot);  // no-op
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  // The genuine handle, bound to the correct slot, still publishes.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c.handle, 4)));
  TEST_ASSERT_EQUAL_UINT16(6, store.view().count);
}

void test_wrong_revision_token_is_rejected() {
  SnapshotStore store;
  FakeFetch f{FetchOutcome::Ok, 7, 7.0f, 0};
  CandidateResult c = store.fetchCandidate(req(7), &fakeFetch, &f);
  // The current settings revision (7) matches the store's candidate, but a token
  // whose *bound* revision was tampered to 8 is not the candidate it claims.
  const CandidateHandle wrong_rev = Access::withRevision(c.handle, 8);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(wrong_rev, 7)));
  TEST_ASSERT_TRUE(store.candidateOutstanding());
  // Distinct from ObsoleteRevision: the genuine token against a newer revision.
  FakeFetch f2{FetchOutcome::Ok, 8, 8.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(7), &fakeFetch, &f2);
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::ObsoleteRevision),
                        pub(store.publishCandidate(c2.handle, 9)));
}

void test_generation_wrap_skips_sentinel_and_distinguishes_candidates() {
  SnapshotStore store(UINT64_MAX - 1U);  // next: MAX, then wrap skips 0 -> 1
  FakeFetch f1{FetchOutcome::Ok, 1, 1.0f, 0};
  CandidateResult c1 = store.fetchCandidate(req(5), &fakeFetch, &f1);
  TEST_ASSERT_TRUE(Access::generation(c1.handle) == UINT64_MAX);
  FakeFetch f2{FetchOutcome::Ok, 2, 2.0f, 0};
  CandidateResult c2 = store.fetchCandidate(req(5), &fakeFetch, &f2);
  // Wrapped past UINT64_MAX: the sequence skips the 0 sentinel, so the new
  // generation is 1, never 0 -- a default handle can never collide with it.
  TEST_ASSERT_TRUE(Access::generation(c2.handle) == 1U);
  // The pre-wrap handle is stale despite the numeric wrap.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(c1.handle, 5)));
  // A default handle (generation 0) is still rejected after the wrap.
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::InvalidHandle),
                        pub(store.publishCandidate(CandidateHandle{}, 5)));
  TEST_ASSERT_EQUAL_INT(pub(PublishResult::Published),
                        pub(store.publishCandidate(c2.handle, 5)));
  TEST_ASSERT_EQUAL_UINT16(2, store.view().count);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_initial_view_is_empty_at_revision_zero);
  RUN_TEST(test_successful_candidate_publishes_and_switches_active);
  RUN_TEST(test_empty_success_publishes);
  RUN_TEST(test_failed_fetch_auto_discards_and_preserves_active);
  RUN_TEST(test_obsolete_revision_preserves_active_and_resolves_candidate);
  RUN_TEST(test_stale_handle_after_supersession_is_invalid);
  RUN_TEST(test_discard_is_idempotent_and_scoped_to_the_handle);
  RUN_TEST(test_active_view_pointer_switches_between_two_slots);
  RUN_TEST(test_revision_wrap_uses_exact_equality);
  RUN_TEST(test_default_handle_is_rejected_and_preserves_active);
  RUN_TEST(test_cross_store_handle_is_rejected);
  RUN_TEST(test_duplicate_publish_is_a_no_op);
  RUN_TEST(test_wrong_slot_token_is_rejected);
  RUN_TEST(test_wrong_revision_token_is_rejected);
  RUN_TEST(test_generation_wrap_skips_sentinel_and_distinguishes_candidates);
  return UNITY_END();
}
