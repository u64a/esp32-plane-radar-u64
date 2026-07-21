#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/portal_auth.h"
#include "core/portal_secrets.h"
#include "core/portal_session.h"
#include "core/url_form.h"

// ADS-B pause/resume model, expressed through the portal/session controller.
//
// The device must never poll ADS-B while a setup session is open, must retain
// its snapshot and backoff across the pause (the model requests a pause but
// never a reset), and must resume with EXACTLY ONE immediate fetch only after
// the STA link is back. These tests drive the whole session lifecycle -- through
// the explicit radio + secrets handshake, the quiesce-before-trial step, and the
// ordered cleanup states -- and assert the ADS-B activity predicate and the
// pause/resume/force actions at each step.

using core::AuthorizedProvisioning;
using core::kDefaultPortalSessionPolicy;
using core::PortalAck;
using core::PortalActions;
using core::PortalInput;
using core::PortalSession;
using core::PortalState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int S(PortalState s) { return static_cast<int>(s); }

// The identity the adapter echoes for the currently outstanding operation.
PortalAck ackOf(const PortalSession& s) {
  return PortalAck{s.session_id, s.trial_id, s.operation_id, s.sta_connection_id};
}

PortalActions upd(PortalSession* s, uint32_t t, PortalInput in,
                  PortalAck ack = {}) {
  return portalSessionUpdate(s, kDefaultPortalSessionPolicy, t, in, ack);
}

// Build a REAL authorized capability through the form + token authentication
// path (fix 6: no test may fabricate an authorized artifact by hand).
AuthorizedProvisioning authFor(uint32_t session_id) {
  const char* token = "0123456789abcdef0123456789abcdef";
  core::ProvisioningForm f;
  char body[160];
  std::snprintf(body, sizeof(body),
                "csrf=%s&ssid=HomeNet&psk=password1&lat=52.37&lon=4.90", token);
  core::urlFormParse(body, static_cast<uint16_t>(std::strlen(body)), 12, &f);
  return core::authenticateProvisioning(
      f, token, static_cast<uint16_t>(std::strlen(token)), session_id);
}

void toOnline(PortalSession* s) {
  portalSessionInit(s);
  upd(s, 0, PortalInput::BootHasCredentials);
  upd(s, 0, PortalInput::StaConnected, ackOf(*s));
}

// Drive from online to a fully prepared SetupSession; returns the session id.
uint32_t toSetup(PortalSession* s) {
  toOnline(s);
  upd(s, 0, PortalInput::ConfigureButton);
  upd(s, 0, PortalInput::PortalRadioReady, ackOf(*s));
  upd(s, 0, PortalInput::SecretsReady, ackOf(*s));
  return s->session_id;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_adsb_paused_in_every_session_phase() {
  // Every "in session" phase reports ADS-B inactive; only StaOnline is active.
  TEST_ASSERT_TRUE(core::portalAdsbActive(PortalState::StaOnline));
  const PortalState session_phases[] = {
      PortalState::PortalRadioPrep, PortalState::PortalAwaitSecrets,
      PortalState::SetupSession,    PortalState::CandidateQuiesce,
      PortalState::CandidateTrial,  PortalState::ReopenRestore,
      PortalState::Commit,          PortalState::SetupCleanup,
      PortalState::TrialCancel,     PortalState::Restore};
  for (PortalState st : session_phases) {
    TEST_ASSERT_TRUE(core::portalInSession(st));
    TEST_ASSERT_FALSE(core::portalAdsbActive(st));
  }
}

void test_configure_pauses_without_resetting_or_fetching() {
  PortalSession s;
  toOnline(&s);
  TEST_ASSERT_TRUE(core::portalAdsbActive(s.state));
  PortalActions a = upd(&s, 1000, PortalInput::ConfigureButton);
  // Pause is requested; resume/force are NOT (snapshot + backoff retained).
  TEST_ASSERT_TRUE(a.pause_adsb);
  TEST_ASSERT_FALSE(a.resume_adsb);
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));
}

void test_no_adsb_activity_through_success_lifecycle() {
  PortalSession s;
  const uint32_t id = toSetup(&s);
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));  // SetupSession
  portalSessionSubmitCandidate(&s, kDefaultPortalSessionPolicy, 100,
                               authFor(id));
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));  // CandidateQuiesce
  upd(&s, 150, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));  // CandidateTrial
  upd(&s, 200, PortalInput::CandidateConnected, ackOf(s));
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));  // Commit
  PortalActions a = upd(&s, 300, PortalInput::CommitDone, ackOf(s));
  TEST_ASSERT_TRUE(core::portalAdsbActive(s.state));   // StaOnline
  TEST_ASSERT_TRUE(a.resume_adsb);
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
}

void test_exactly_one_forced_fetch_across_close_and_reconnect() {
  PortalSession s;
  toSetup(&s);
  int forced = 0;
  int resumes = 0;
  // Cancel -> ordered cleanup: stop the listener first, wait for the quiesce ack,
  // THEN restore. No forced fetch is emitted before the STA link is back.
  PortalActions a = upd(&s, 1000, PortalInput::SessionCancel);  // SetupCleanup
  forced += a.force_immediate_adsb_fetch ? 1 : 0;
  resumes += a.resume_adsb ? 1 : 0;
  a = upd(&s, 1050, PortalInput::PortalQuiesced, ackOf(s));  // Restore
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  forced += a.force_immediate_adsb_fetch ? 1 : 0;
  resumes += a.resume_adsb ? 1 : 0;
  a = upd(&s, 1100, PortalInput::RestoreDone, ackOf(s));  // StaConnecting
  forced += a.force_immediate_adsb_fetch ? 1 : 0;
  resumes += a.resume_adsb ? 1 : 0;
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));
  a = upd(&s, 1200, PortalInput::StaConnected, ackOf(s));  // StaOnline
  forced += a.force_immediate_adsb_fetch ? 1 : 0;
  resumes += a.resume_adsb ? 1 : 0;
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_EQUAL_INT(1, forced);
  TEST_ASSERT_EQUAL_INT(1, resumes);
  TEST_ASSERT_TRUE(core::portalAdsbActive(s.state));
}

void test_candidate_failure_keeps_adsb_paused_and_preserves_credentials() {
  PortalSession s;
  const uint32_t id = toSetup(&s);
  portalSessionSubmitCandidate(&s, kDefaultPortalSessionPolicy, 100,
                               authFor(id));
  upd(&s, 150, PortalInput::PortalQuiesced, ackOf(s));       // CandidateTrial
  PortalActions a = upd(&s, 200, PortalInput::CandidateFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));  // reopen restore
  TEST_ASSERT_FALSE(core::portalAdsbActive(s.state));
  TEST_ASSERT_FALSE(a.commit_candidate_flash);
  TEST_ASSERT_TRUE(s.has_old_config);
}

void test_reconnect_only_forces_fetch_when_pending() {
  PortalSession s;
  toOnline(&s);
  upd(&s, 0, PortalInput::StaLost, ackOf(s));  // StaConnecting (fresh generation)
  PortalActions a = upd(&s, 100, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
}

void test_first_boot_close_has_no_pending_resume() {
  // First boot with no stored config: closing setup must NOT arm a resume/fetch;
  // ADS-B stays paused until a real STA link appears (after a retry generation).
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootNoCredentials);  // PortalRadioPrep (no old config)
  upd(&s, 1000, PortalInput::SessionCancel);   // SetupCleanup (stop first)
  upd(&s, 1050, PortalInput::PortalQuiesced, ackOf(s));  // -> StaOfflineIdle
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(s.pending_immediate_fetch);
  upd(&s, 4000, PortalInput::StaRetry);  // adapter mints a reconnect generation
  PortalActions a = upd(&s, 5000, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_FALSE(a.resume_adsb);
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_adsb_paused_in_every_session_phase);
  RUN_TEST(test_configure_pauses_without_resetting_or_fetching);
  RUN_TEST(test_no_adsb_activity_through_success_lifecycle);
  RUN_TEST(test_exactly_one_forced_fetch_across_close_and_reconnect);
  RUN_TEST(test_candidate_failure_keeps_adsb_paused_and_preserves_credentials);
  RUN_TEST(test_reconnect_only_forces_fetch_when_pending);
  RUN_TEST(test_first_boot_close_has_no_pending_resume);
  return UNITY_END();
}
