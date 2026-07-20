#include <unity.h>

#include <cstdint>

#include "core/poll_policy.h"
#include "core/radar_data_state.h"
#include "core/radar_status.h"
#include "core/settings_events.h"
#include "services/adsb_fetch.h"
#include "services/adsb_snapshot_store.h"

// Integration-style native model test. src/main.cpp is Arduino-only, so it cannot
// be native-tested directly; this composes the exact same Arduino-free seams
// (SnapshotStore + RadarDataState + AdsbPollState + SettingsState + the poll/
// publish/outcome helpers) in the same order main.cpp's fetch/publish flow does,
// and drives the Phase 6 critical revision-race sequence plus the Wi-Fi-drop and
// visual-only invariants end to end.

using namespace services::adsb;
using core::AdsbPollState;
using core::PollOutcome;
using core::RadarDataMode;
using core::RadarDataState;
using core::RadarDataView;
using core::RadarStatusBadge;
using core::RadarStatusPlan;
using core::SettingsState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

constexpr core::AdsbPollPolicy P = core::kDefaultAdsbPollPolicy;
constexpr core::RadarFreshnessPolicy F = core::kDefaultRadarFreshnessPolicy;

// Programmable fetch seam handed to SnapshotStore::fetchCandidate.
struct FakeFetch {
  FetchOutcome outcome = FetchOutcome::Ok;
  uint16_t count = 0;
  float marker = 0.0f;
};

FetchResult fakeFetch(const FetchRequest& request, AircraftSnapshot& out,
                      void* ctx) {
  FakeFetch* f = static_cast<FakeFetch*>(ctx);
  FetchResult r{};
  r.outcome = f->outcome;
  if (f->outcome == FetchOutcome::Ok) {
    r.http_status = 200;
    r.aircraft_count = f->count;
    out.count = f->count;
    out.settings_revision = request.settings_revision;
    if (f->count > 0) {
      out.aircraft[0].lat = f->marker;
    }
  } else {
    r.http_status = -1;
  }
  return r;
}

FetchRequest req(uint32_t revision) {
  return FetchRequest{10.0, 20.0, 5.0f, revision};
}

int mode(RadarDataMode m) { return static_cast<int>(m); }
int result(PublishResult r) { return static_cast<int>(r); }
int outcome(PollOutcome o) { return static_cast<int>(o); }

// Mirrors main.cpp: only a real publication advances freshness/last-success.
void applyPublish(RadarDataState* data, PublishResult publish, uint32_t revision,
                  uint32_t now_ms) {
  if (publish == PublishResult::Published) {
    core::radarDataSuccess(data, revision, now_ms);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_critical_revision_race_sequence() {
  SnapshotStore store;
  SettingsState settings{};
  RadarDataState data{};
  AdsbPollState poll{};

  // --- Boot at revision 7, radar shown. ----------------------------------
  for (int i = 0; i < 7; ++i) {
    core::settingsQueryChanged(&settings);
  }
  TEST_ASSERT_EQUAL_UINT32(7, core::settingsRevision(settings));
  core::settingsConsumeQueryChange(&settings);  // adopt the boot-time latch
  core::radarDataStart(&data, 1000);
  core::radarDataRevisionChanged(&data, core::settingsRevision(settings), 1000);
  core::adsbRadarDisplayed(&poll);

  // --- Snapshot A published (revision 7, 3 aircraft) -> Live. -------------
  const uint32_t rev_a = core::settingsRevision(settings);  // 7
  core::adsbFetchStarted(&poll);
  FakeFetch fa{FetchOutcome::Ok, 3, 7.5f};
  CandidateResult A = store.fetchCandidate(req(rev_a), &fakeFetch, &fa);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok),
                        static_cast<int>(A.fetch.outcome));
  const PublishResult pa =
      store.publishCandidate(A.handle, core::settingsRevision(settings));
  TEST_ASSERT_EQUAL_INT(result(PublishResult::Published), result(pa));
  applyPublish(&data, pa, core::settingsRevision(settings), 2000);
  core::adsbFetchCompleted(&poll, 2000, P, pollOutcomeForPublish(pa), false, 0);
  core::radarDataAdvance(&data, 2000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live), mode(data.mode));
  TEST_ASSERT_EQUAL_UINT16(3, store.view().count);
  TEST_ASSERT_EQUAL_UINT32(7, store.view().settings_revision);

  // --- Request B starts for revision 7. ----------------------------------
  const uint32_t rev_b = core::settingsRevision(settings);  // 7
  core::adsbFetchStarted(&poll);
  FakeFetch fb{FetchOutcome::Ok, 5, 9.0f};
  CandidateResult B = store.fetchCandidate(req(rev_b), &fakeFetch, &fb);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok),
                        static_cast<int>(B.fetch.outcome));

  // --- Mid-fetch: an effective query change advances to revision 8. ------
  // (In main this is a range tap / portal save consumed after the fetch's
  //  wifiLoop, before publication.)
  core::settingsQueryChanged(&settings);
  TEST_ASSERT_TRUE(core::settingsConsumeQueryChange(&settings));
  const uint32_t rev8 = core::settingsRevision(settings);  // 8
  core::radarDataRevisionChanged(&data, rev8, 3000);  // reset to Loading @ rev8
  core::adsbSettingsChanged(&poll);  // force one immediate fetch, streak intact

  // --- B returns Ok but publishes against the now-current revision 8. -----
  const PublishResult pb =
      store.publishCandidate(B.handle, core::settingsRevision(settings));
  TEST_ASSERT_EQUAL_INT(result(PublishResult::ObsoleteRevision), result(pb));
  const PollOutcome ob = pollOutcomeForPublish(pb);
  TEST_ASSERT_EQUAL_INT(outcome(PollOutcome::Obsolete), outcome(ob));
  applyPublish(&data, pb, core::settingsRevision(settings), 4000);  // no-op: rejected
  core::adsbFetchCompleted(&poll, 4000, P, ob, false, 0);

  // A remains active in the store byte-for-byte, but is hidden by the mismatch.
  TEST_ASSERT_EQUAL_UINT16(3, store.view().count);
  TEST_ASSERT_EQUAL_UINT32(7, store.view().settings_revision);
  core::radarDataAdvance(&data, 4000, F);
  const RadarDataView v = core::radarDataView(data, 4000);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Loading), mode(v.mode));
  TEST_ASSERT_EQUAL_UINT32(8, v.settings_revision);
  TEST_ASSERT_FALSE(data.has_success);  // freshness last-success unchanged by B
  const RadarStatusPlan hidden = core::radarStatusPlan(
      v.mode, v.age_seconds, v.show_aircraft, v.settings_revision,
      store.view().settings_revision, /*wifi=*/true, /*phase=*/0);
  TEST_ASSERT_FALSE(hidden.draw_aircraft);  // revision mismatch hides A

  // --- rev8 fetch immediately due; obsolete completion left the streak. ---
  TEST_ASSERT_TRUE(core::adsbFetchDue(poll, 4000));
  TEST_ASSERT_EQUAL_UINT8(0, poll.transient_streak);

  // --- Empty rev8 success publishes Live. --------------------------------
  const uint32_t rev_c = core::settingsRevision(settings);  // 8
  core::adsbFetchStarted(&poll);
  FakeFetch fc{FetchOutcome::Ok, 0, 0.0f};  // zero aircraft
  CandidateResult C = store.fetchCandidate(req(rev_c), &fakeFetch, &fc);
  const PublishResult pc =
      store.publishCandidate(C.handle, core::settingsRevision(settings));
  TEST_ASSERT_EQUAL_INT(result(PublishResult::Published), result(pc));
  applyPublish(&data, pc, core::settingsRevision(settings), 5000);
  core::adsbFetchCompleted(&poll, 5000, P, pollOutcomeForPublish(pc), false, 0);
  core::radarDataAdvance(&data, 5000, F);
  TEST_ASSERT_EQUAL_INT(mode(RadarDataMode::Live), mode(data.mode));
  TEST_ASSERT_EQUAL_UINT16(0, store.view().count);
  TEST_ASSERT_EQUAL_UINT32(8, store.view().settings_revision);
  // Published zero-aircraft data is Live and drawn (the renderer just draws none).
  const RadarDataView vlive = core::radarDataView(data, 5000);
  const RadarStatusPlan live_plan = core::radarStatusPlan(
      vlive.mode, vlive.age_seconds, vlive.show_aircraft, vlive.settings_revision,
      store.view().settings_revision, /*wifi=*/true, /*phase=*/0);
  TEST_ASSERT_TRUE(live_plan.draw_aircraft);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RadarStatusBadge::None),
                        static_cast<int>(live_plan.badge));
}

void test_wifi_down_failure_does_not_increment_transient_streak() {
  SnapshotStore store;
  AdsbPollState poll{};
  core::adsbRadarDisplayed(&poll);

  // Seed a genuine (connected) transient failure: the streak advances to 1.
  core::adsbFetchStarted(&poll);
  FakeFetch bad{FetchOutcome::Timeout, 0, 0.0f};
  CandidateResult f1 = store.fetchCandidate(req(0), &fakeFetch, &bad);
  TEST_ASSERT_FALSE(f1.handle.valid());  // failed fetch auto-discards
  PollOutcome o1 = pollOutcomeFor(f1.fetch.outcome);              // Transient
  o1 = core::effectiveOutcomeAtCompletion(o1, /*wifi=*/true);     // stays Transient
  core::adsbFetchCompleted(&poll, 1000, P, o1, false, 0);
  TEST_ASSERT_EQUAL_UINT8(1, poll.transient_streak);

  // Now a fetch is aborted because Wi-Fi dropped: at completion Wi-Fi is down.
  core::adsbFetchStarted(&poll);
  CandidateResult f2 = store.fetchCandidate(req(0), &fakeFetch, &bad);
  PollOutcome o2 = pollOutcomeFor(f2.fetch.outcome);             // Transient
  o2 = core::effectiveOutcomeAtCompletion(o2, /*wifi=*/false);   // -> Obsolete
  core::adsbFetchCompleted(&poll, 2000, P, o2, false, 0);
  TEST_ASSERT_EQUAL_UINT8(1, poll.transient_streak);      // NOT incremented
  TEST_ASSERT_EQUAL_UINT32(3000, poll.next_interval_ms);  // obsolete/pause cadence
  TEST_ASSERT_EQUAL_UINT16(0, store.view().count);        // nothing published
}

void test_wifi_flap_during_fetch_auto_reconnected_becomes_obsolete() {
  // The exact blocker: Wi-Fi drops AND auto-reconnects entirely inside one
  // blocking fetch. Level-based wifiConnected() reads true before and after, so
  // the main loop's Wi-Fi edge is missed; only the disconnect sequence reveals
  // the flap. The premature TLS/HTTP EOF now surfaces as TransportFailure. The
  // flap must (a) NOT advance the transient/permanent streak and (b) force one
  // immediate refresh afterward, even though the link is already back up.
  SnapshotStore store;
  AdsbPollState poll{};
  core::adsbRadarDisplayed(&poll);

  // Baseline: a clean published success -> normal 3 s cadence, streak 0.
  core::adsbFetchStarted(&poll);
  FakeFetch ok{FetchOutcome::Ok, 2, 1.0f};
  CandidateResult a = store.fetchCandidate(req(0), &fakeFetch, &ok);
  const PublishResult pa = store.publishCandidate(a.handle, 0);
  core::adsbFetchCompleted(&poll, 1000, P, pollOutcomeForPublish(pa), false, 0);
  TEST_ASSERT_FALSE(core::adsbFetchDue(poll, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, poll.transient_streak);

  // Capture the disconnect sequence before the fetch (as serviceAdsb does).
  uint32_t wifi_seq = 7;  // stands in for wifiDisconnectSeq()
  const uint32_t seq_before = wifi_seq;

  core::adsbFetchStarted(&poll);
  FakeFetch flap{FetchOutcome::TransportFailure, 0, 0.0f};  // premature TLS/HTTP EOF
  CandidateResult b = store.fetchCandidate(req(0), &fakeFetch, &flap);
  TEST_ASSERT_FALSE(b.handle.valid());  // failed fetch: nothing to publish

  wifi_seq += 1;  // exactly one ARDUINO_EVENT_WIFI_STA_DISCONNECTED fired mid-fetch
  const bool flapped = core::disconnectSeqChanged(seq_before, wifi_seq);
  TEST_ASSERT_TRUE(flapped);

  // main.cpp completion pipeline with the link already auto-reconnected.
  PollOutcome base = pollOutcomeFor(b.fetch.outcome);                 // Transient
  base = core::effectiveOutcomeAtCompletion(base, /*wifi_up=*/true);  // stays Transient
  const PollOutcome eff = effectiveOutcomeAfterFlap(
      b.fetch.outcome, base, /*published_success=*/false, flapped);
  TEST_ASSERT_EQUAL_INT(outcome(PollOutcome::Obsolete), outcome(eff));

  // Force the immediate BEFORE completion, exactly as serviceAdsb does, so the
  // in-flight sequence logic preserves it across this (older) completion.
  core::adsbSettingsChanged(&poll);
  core::adsbFetchCompleted(&poll, 5000, P, eff, false, 0);

  TEST_ASSERT_EQUAL_UINT8(0, poll.transient_streak);       // streak untouched
  TEST_ASSERT_EQUAL_UINT32(3000, poll.next_interval_ms);   // pause (not 5-min permanent)
  TEST_ASSERT_TRUE(core::adsbFetchDue(poll, 5000));        // one forced immediate refresh
  TEST_ASSERT_EQUAL_UINT16(2, store.view().count);         // prior snapshot preserved
}

void test_wifi_flap_with_published_success_stays_success_and_forces_immediate() {
  // A flap during a fetch that STILL fully publishes a success: the success must
  // stand (freshness advances) yet the latched disconnect still forces one
  // immediate refresh afterward.
  SnapshotStore store;
  AdsbPollState poll{};
  core::adsbRadarDisplayed(&poll);
  core::adsbFetchStarted(&poll);
  core::adsbFetchCompleted(&poll, 1000, P, PollOutcome::Success, false, 0);
  TEST_ASSERT_FALSE(core::adsbFetchDue(poll, 1000));  // initial immediate consumed

  uint32_t wifi_seq = 100;
  const uint32_t seq_before = wifi_seq;
  core::adsbFetchStarted(&poll);
  FakeFetch ok{FetchOutcome::Ok, 4, 2.0f};
  CandidateResult c = store.fetchCandidate(req(0), &fakeFetch, &ok);
  const PublishResult pc = store.publishCandidate(c.handle, 0);
  TEST_ASSERT_EQUAL_INT(result(PublishResult::Published), result(pc));
  wifi_seq += 1;  // a disconnect fired mid-fetch, but the read still completed
  const bool flapped = core::disconnectSeqChanged(seq_before, wifi_seq);

  PollOutcome base = pollOutcomeForPublish(pc);            // Success
  base = core::effectiveOutcomeAtCompletion(base, true);
  const PollOutcome eff = effectiveOutcomeAfterFlap(
      FetchOutcome::Ok, base, /*published_success=*/true, flapped);
  TEST_ASSERT_EQUAL_INT(outcome(PollOutcome::Success), outcome(eff));

  core::adsbSettingsChanged(&poll);  // force immediate before completion
  core::adsbFetchCompleted(&poll, 5000, P, eff, false, 0);
  TEST_ASSERT_EQUAL_UINT8(0, poll.transient_streak);
  TEST_ASSERT_EQUAL_UINT32(3000, poll.next_interval_ms);  // success cadence
  TEST_ASSERT_TRUE(core::adsbFetchDue(poll, 5000));       // flap forced one immediate
  TEST_ASSERT_EQUAL_UINT16(4, store.view().count);        // the success published
}

void test_no_flap_malformed_parse_error_stays_permanent() {
  // Without a flap, a genuinely malformed response stays Permanent (5-min backoff)
  // -- the flap path must never soften real parse failures.
  SnapshotStore store;
  AdsbPollState poll{};
  core::adsbRadarDisplayed(&poll);
  core::adsbFetchStarted(&poll);
  FakeFetch bad{FetchOutcome::ParseError, 0, 0.0f};
  CandidateResult b = store.fetchCandidate(req(0), &fakeFetch, &bad);
  const bool flapped = core::disconnectSeqChanged(42, 42);  // no disconnect fired
  TEST_ASSERT_FALSE(flapped);

  PollOutcome base = pollOutcomeFor(b.fetch.outcome);       // Permanent
  base = core::effectiveOutcomeAtCompletion(base, true);
  const PollOutcome eff = effectiveOutcomeAfterFlap(
      b.fetch.outcome, base, /*published_success=*/false, flapped);
  TEST_ASSERT_EQUAL_INT(outcome(PollOutcome::Permanent), outcome(eff));

  core::adsbFetchCompleted(&poll, 2000, P, eff, false, 0);
  TEST_ASSERT_EQUAL_UINT32(300000, poll.next_interval_ms);  // 5-min permanent backoff
}

void test_visual_only_change_does_not_revise_query() {
  SettingsState settings{};
  core::settingsQueryChanged(&settings);  // an effective change -> revision 1
  core::settingsConsumeQueryChange(&settings);
  const uint32_t before = core::settingsRevision(settings);  // 1

  core::settingsVisualChanged(&settings);  // units / runway overlay toggle
  TEST_ASSERT_EQUAL_UINT32(before, core::settingsRevision(settings));  // unchanged
  TEST_ASSERT_FALSE(core::settingsConsumeQueryChange(&settings));  // no query event
  TEST_ASSERT_TRUE(core::settingsConsumeVisualChange(&settings));  // redraw-only latch
  TEST_ASSERT_FALSE(core::settingsConsumeVisualChange(&settings));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_critical_revision_race_sequence);
  RUN_TEST(test_wifi_down_failure_does_not_increment_transient_streak);
  RUN_TEST(test_wifi_flap_during_fetch_auto_reconnected_becomes_obsolete);
  RUN_TEST(test_wifi_flap_with_published_success_stays_success_and_forces_immediate);
  RUN_TEST(test_no_flap_malformed_parse_error_stays_permanent);
  RUN_TEST(test_visual_only_change_does_not_revise_query);
  return UNITY_END();
}
