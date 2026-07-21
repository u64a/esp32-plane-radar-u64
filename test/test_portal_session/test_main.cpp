#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/portal_auth.h"
#include "core/portal_secrets.h"
#include "core/portal_session.h"
#include "core/url_form.h"

using core::AuthorizedProvisioning;
using core::kDefaultPortalSessionPolicy;
using core::PortalAck;
using core::PortalActions;
using core::PortalInput;
using core::PortalSession;
using core::PortalSessionPolicy;
using core::PortalState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int S(PortalState s) { return static_cast<int>(s); }
int IN(PortalInput i) { return static_cast<int>(i); }

// The identity the adapter echoes for the currently outstanding operation. Now
// carries the STA link generation too, so link/candidate events match.
PortalAck ackOf(const PortalSession& s) {
  return PortalAck{s.session_id, s.trial_id, s.operation_id, s.sta_connection_id};
}

PortalActions upd(PortalSession* s, uint32_t t, PortalInput in,
                  PortalAck ack = {}) {
  return portalSessionUpdate(s, kDefaultPortalSessionPolicy, t, in, ack);
}
PortalActions updP(PortalSession* s, const PortalSessionPolicy& p, uint32_t t,
                   PortalInput in, PortalAck ack = {}) {
  return portalSessionUpdate(s, p, t, in, ack);
}

// Build a REAL authorized capability through the form + token authentication
// path (fix 6: no test may fabricate an authorized artifact by hand). With
// ok=false the form carries a mismatched token, so authentication fails and the
// artifact is unauthorized -- but still session-bound for diagnostics.
AuthorizedProvisioning authFor(uint32_t session_id, bool ok = true) {
  const char* token = "0123456789abcdef0123456789abcdef";
  const char* form_token = ok ? token : "fedcba9876543210fedcba9876543210";
  core::ProvisioningForm f;
  char body[160];
  std::snprintf(body, sizeof(body),
                "csrf=%s&ssid=HomeNet&psk=password1&lat=52.37&lon=4.90",
                form_token);
  core::urlFormParse(body, static_cast<uint16_t>(std::strlen(body)), 12, &f);
  return core::authenticateProvisioning(
      f, token, static_cast<uint16_t>(std::strlen(token)), session_id);
}

PortalActions submit(PortalSession* s, uint32_t t,
                     const AuthorizedProvisioning& sub) {
  return portalSessionSubmitCandidate(s, kDefaultPortalSessionPolicy, t, sub);
}

// Per-transition safety invariants (fix 9 + fixes 3/5). Asserted on every step.
void assertInvariants(PortalState pre, PortalInput in, const PortalActions& a,
                      const PortalSession& post) {
  // Destructive erase only ever from a confirmed erase gesture.
  if (a.factory_erase) {
    TEST_ASSERT_EQUAL_INT(IN(PortalInput::EraseConfirmed), IN(in));
  }
  // A candidate-connect request implies the portal is quiesced: it comes from
  // CandidateQuiesce into the listener-inactive CandidateTrial.
  if (a.begin_candidate_trial) {
    TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(pre));
    TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(post.state));
    TEST_ASSERT_FALSE(core::portalListenerActive(post.state));
  }
  // While the listener is active, never request an STA or candidate connect.
  if (core::portalListenerActive(pre)) {
    TEST_ASSERT_FALSE(a.begin_candidate_trial);
    TEST_ASSERT_FALSE(a.start_sta_connect);
  }
  // Flash is written only on a real candidate connect (old creds never touched
  // before CandidateConnected).
  if (a.commit_candidate_flash) {
    TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(pre));
    TEST_ASSERT_EQUAL_INT(IN(PortalInput::CandidateConnected), IN(in));
  }
  // Ordered rollback (fixes 3/5): never emit restore or STA-reconnect in the same
  // transition as an unacknowledged listener stop or candidate cancellation.
  if (a.restore_old_ram_config) {
    TEST_ASSERT_FALSE(a.stop_listener);
    TEST_ASSERT_FALSE(a.cancel_candidate_trial);
    // Reopen restore is operation-bound: never combine restore-old with start-AP.
    TEST_ASSERT_FALSE(a.start_ap_radio);
  }
  if (a.start_sta_connect) {
    TEST_ASSERT_FALSE(a.stop_listener);
    TEST_ASSERT_FALSE(a.cancel_candidate_trial);
  }
  // The exposed ack identity is coherent with the outstanding operation.
  if (a.ack_operation_id != 0) {
    TEST_ASSERT_EQUAL_UINT32(post.operation_id, a.ack_operation_id);
    TEST_ASSERT_EQUAL_UINT32(post.session_id, a.ack_session_id);
    TEST_ASSERT_EQUAL_UINT32(post.trial_id, a.ack_trial_id);
  }
  // Every STA attempt (stored/restored connect or candidate trial) exposes a
  // fresh nonzero link generation equal to the session's current one.
  if (a.start_sta_connect || a.begin_candidate_trial) {
    TEST_ASSERT_NOT_EQUAL(0, a.ack_sta_connection_id);
    TEST_ASSERT_EQUAL_UINT32(post.sta_connection_id, a.ack_sta_connection_id);
  }
}

PortalActions step(PortalSession* s, uint32_t t, PortalInput in,
                   PortalAck ack = {}) {
  PortalState pre = s->state;
  PortalActions a = upd(s, t, in, ack);
  assertInvariants(pre, in, a, *s);
  return a;
}

PortalActions stepSubmit(PortalSession* s, uint32_t t,
                         const AuthorizedProvisioning& sub) {
  PortalActions a = submit(s, t, sub);
  // Submit never erases, connects a candidate, or writes flash directly.
  TEST_ASSERT_FALSE(a.factory_erase);
  TEST_ASSERT_FALSE(a.begin_candidate_trial);
  TEST_ASSERT_FALSE(a.start_sta_connect);
  TEST_ASSERT_FALSE(a.commit_candidate_flash);
  return a;
}

void toOnline(PortalSession* s) {
  portalSessionInit(s);
  upd(s, 0, PortalInput::BootHasCredentials);  // -> StaConnecting (mints STA id)
  upd(s, 0, PortalInput::StaConnected, ackOf(*s));  // echo the STA generation
}

// Drive a full, fully-prepared SetupSession from online at time t0. Returns the
// session id. Uses the identity-bound handshake: radio ready -> secrets ready.
uint32_t openSetupFromOnline(PortalSession* s, uint32_t t0 = 0) {
  toOnline(s);
  step(s, t0, PortalInput::ConfigureButton);                 // PortalRadioPrep
  step(s, t0, PortalInput::PortalRadioReady, ackOf(*s));      // PortalAwaitSecrets
  step(s, t0, PortalInput::SecretsReady, ackOf(*s));          // SetupSession
  return s->session_id;
}

// Drive a fresh machine to `target` with the session opened at `open_ms`.
void driveTo(PortalSession* s, PortalState target, uint32_t open_ms) {
  toOnline(s);
  step(s, open_ms, PortalInput::ConfigureButton);            // PortalRadioPrep
  if (target == PortalState::PortalRadioPrep) return;
  step(s, open_ms, PortalInput::PortalRadioReady, ackOf(*s));  // PortalAwaitSecrets
  if (target == PortalState::PortalAwaitSecrets) return;
  step(s, open_ms, PortalInput::SecretsReady, ackOf(*s));      // SetupSession
  if (target == PortalState::SetupSession) return;
  stepSubmit(s, open_ms, authFor(s->session_id));            // CandidateQuiesce
  if (target == PortalState::CandidateQuiesce) return;
  step(s, open_ms, PortalInput::PortalQuiesced, ackOf(*s));    // CandidateTrial
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- boot paths -------------------------------------------------------------

void test_boot_no_credentials_starts_radio_prep_not_listener() {
  PortalSession s;
  portalSessionInit(&s);
  PortalActions a = upd(&s, 0, PortalInput::BootNoCredentials);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_TRUE(a.pause_adsb);
  TEST_ASSERT_TRUE(a.disconnect_sta);
  TEST_ASSERT_TRUE(a.start_ap_radio);
  TEST_ASSERT_FALSE(a.start_listener);            // NOT yet
  TEST_ASSERT_FALSE(s.secrets_active);            // NOT active before ack
  TEST_ASSERT_FALSE(s.has_old_config);
  TEST_ASSERT_NOT_EQUAL(0, a.ack_operation_id);   // radio-prep op exposed
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
}

void test_boot_with_credentials_connects_without_listener() {
  PortalSession s;
  portalSessionInit(&s);
  PortalActions a = upd(&s, 0, PortalInput::BootHasCredentials);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  TEST_ASSERT_TRUE(a.start_sta_connect);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(s.has_old_config);
}

void test_boot_connect_success_no_forced_fetch() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootHasCredentials);
  PortalActions a = upd(&s, 0, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
}

void test_failed_saved_wifi_goes_offline_idle_never_opens_listener() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootHasCredentials);
  PortalActions a = upd(&s, 0, PortalInput::StaConnectFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
}

// --- radio + secrets sequencing (fix 1) -------------------------------------

void test_setup_open_sequences_radio_then_secrets_then_listener() {
  PortalSession s;
  toOnline(&s);
  // 1) Configure: pause ADS-B, disconnect STA, init AP radio -- no listener.
  PortalActions a = step(&s, 1000, PortalInput::ConfigureButton);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_TRUE(a.pause_adsb);
  TEST_ASSERT_TRUE(a.disconnect_sta);
  TEST_ASSERT_TRUE(a.start_ap_radio);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_FALSE(a.request_secret_generation);
  TEST_ASSERT_FALSE(s.secrets_active);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  // 2) Radio ready: request secret generation -- still no listener/secrets.
  a = step(&s, 1000, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalAwaitSecrets), S(s.state));
  TEST_ASSERT_TRUE(a.request_secret_generation);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_FALSE(s.secrets_active);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  // 3) Secrets ready: NOW the listener starts and secrets become active.
  a = step(&s, 1000, PortalInput::SecretsReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_TRUE(a.start_listener);
  TEST_ASSERT_TRUE(s.secrets_active);
  TEST_ASSERT_TRUE(core::portalListenerActive(s.state));
}

void test_secrets_failure_fails_closed_without_listener() {
  PortalSession s;
  toOnline(&s);
  step(&s, 0, PortalInput::ConfigureButton);
  step(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalAwaitSecrets), S(s.state));
  PortalActions a = step(&s, 0, PortalInput::SecretsFailed, ackOf(s));
  // Fail closed: never a listener; stop the AP radio and wait for the ack before
  // restoring (ordered cleanup).
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(a.stop_listener);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);  // NOT until the stop is acked
  TEST_ASSERT_FALSE(s.secrets_active);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  // The stop is acknowledged, THEN the old config is restored.
  a = step(&s, 0, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_secrets_not_active_before_acknowledgement() {
  PortalSession s;
  toOnline(&s);
  step(&s, 0, PortalInput::ConfigureButton);
  TEST_ASSERT_FALSE(s.secrets_active);
  step(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_FALSE(s.secrets_active);  // still not active after radio ready
  step(&s, 0, PortalInput::SecretsReady, ackOf(s));
  TEST_ASSERT_TRUE(s.secrets_active);   // active only after the ack
}

// --- identity binding (fix 1) -----------------------------------------------

void test_stale_radio_ready_is_ignored() {
  PortalSession s;
  toOnline(&s);
  step(&s, 0, PortalInput::ConfigureButton);
  PortalAck good = ackOf(s);
  PortalAck wrong_op = {good.session_id, good.trial_id, good.operation_id + 99};
  step(&s, 0, PortalInput::PortalRadioReady, wrong_op);  // wrong operation_id
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));  // ignored
  PortalAck wrong_sess = {good.session_id + 7, good.trial_id, good.operation_id};
  step(&s, 0, PortalInput::PortalRadioReady, wrong_sess);  // wrong session_id
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));  // ignored
  step(&s, 0, PortalInput::PortalRadioReady, good);        // correct identity
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalAwaitSecrets), S(s.state));
}

void test_stale_secrets_ready_is_ignored() {
  PortalSession s;
  toOnline(&s);
  step(&s, 0, PortalInput::ConfigureButton);
  step(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  PortalAck good = ackOf(s);
  PortalAck wrong = {good.session_id, good.trial_id, good.operation_id + 5};
  step(&s, 0, PortalInput::SecretsReady, wrong);  // wrong operation_id
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalAwaitSecrets), S(s.state));
  TEST_ASSERT_FALSE(s.secrets_active);
  step(&s, 0, PortalInput::SecretsReady, good);   // correct identity
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
}

// The headline adversarial case (fix 1): a session re-enters radio preparation
// with the SAME session_id after a candidate failure, and a delayed duplicate
// PortalRadioReady from the FIRST preparation is replayed. It must be rejected by
// operation_id, so it can never start the listener before the new teardown.
void test_stale_first_radio_ready_replayed_on_reopen_is_ignored() {
  PortalSession s;
  toOnline(&s);
  step(&s, 0, PortalInput::ConfigureButton);            // PortalRadioPrep (op1)
  PortalAck first_ready = ackOf(s);                     // FIRST preparation's ack
  step(&s, 0, PortalInput::PortalRadioReady, first_ready);  // PortalAwaitSecrets
  step(&s, 0, PortalInput::SecretsReady, ackOf(s));     // SetupSession
  const uint32_t id = s.session_id;
  stepSubmit(&s, 1000, authFor(id));                   // CandidateQuiesce
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  step(&s, 2000, PortalInput::CandidateFailed, ackOf(s));  // -> ReopenRestore
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  step(&s, 2000, PortalInput::RestoreDone, ackOf(s));   // reopen -> PortalRadioPrep
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_TRUE(s.secrets_active);
  TEST_ASSERT_EQUAL_UINT32(id, s.session_id);          // SAME session id
  // Replay the first preparation's PortalRadioReady: same session_id, stale op.
  PortalActions a = step(&s, 2000, PortalInput::PortalRadioReady, first_ready);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));  // ignored!
  TEST_ASSERT_FALSE(a.start_listener);
  // Only the CURRENT preparation's ack returns to the listener.
  a = step(&s, 2000, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_TRUE(a.start_listener);
}

void test_stale_completion_from_prior_trial_no_commit_or_reopen() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  // Trial 1.
  stepSubmit(&s, 1000, authFor(id));                          // CandidateQuiesce
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));       // CandidateTrial
  PortalAck trial1 = ackOf(s);
  step(&s, 2000, PortalInput::CandidateFailed, ackOf(s));      // -> ReopenRestore
  step(&s, 2000, PortalInput::RestoreDone, ackOf(s));         // reopen prep
  step(&s, 2000, PortalInput::PortalRadioReady, ackOf(s));     // back to SetupSession
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  // Trial 2.
  stepSubmit(&s, 3000, authFor(id));                          // CandidateQuiesce
  step(&s, 3000, PortalInput::PortalQuiesced, ackOf(s));       // CandidateTrial
  PortalAck trial2 = ackOf(s);
  TEST_ASSERT_NOT_EQUAL(trial1.operation_id, trial2.operation_id);
  // Delayed completions from trial 1 must be ignored: no commit, no reopen.
  PortalActions a = step(&s, 3100, PortalInput::CandidateConnected, trial1);
  TEST_ASSERT_FALSE(a.commit_candidate_flash);
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));
  a = step(&s, 3100, PortalInput::CandidateFailed, trial1);
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));  // no reopen
  a = step(&s, 3100, PortalInput::CommitDone, trial1);
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));
}

void test_stale_completion_from_prior_session_ignored() {
  PortalSession s;
  const uint32_t id1 = openSetupFromOnline(&s, 0);
  upd(&s, 0, PortalInput::SessionCancel);               // -> SetupCleanup
  upd(&s, 0, PortalInput::PortalQuiesced, ackOf(s));    // -> Restore
  upd(&s, 0, PortalInput::RestoreDone, ackOf(s));       // StaConnecting
  upd(&s, 0, PortalInput::StaConnected, ackOf(s));      // StaOnline
  PortalAck sess1 = {id1, 0, s.operation_id, 0};
  // Open session 2 WITHOUT re-initialising (session_id must advance past id1).
  step(&s, 0, PortalInput::ConfigureButton);            // PortalRadioPrep
  step(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  step(&s, 0, PortalInput::SecretsReady, ackOf(s));     // SetupSession
  const uint32_t id2 = s.session_id;
  TEST_ASSERT_NOT_EQUAL(id1, id2);
  // A late completion from session 1 must not disturb session 2.
  step(&s, 0, PortalInput::SecretsReady, sess1);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
}

// --- candidate quiesce before trial -----------------------------------------

void test_candidate_submit_quiesces_before_trial() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  PortalActions a = stepSubmit(&s, 1000, authFor(id));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(s.state));
  TEST_ASSERT_TRUE(a.stop_listener);
  TEST_ASSERT_FALSE(a.begin_candidate_trial);       // NOT yet
  TEST_ASSERT_TRUE(s.has_candidate);
  TEST_ASSERT_NOT_EQUAL(0, s.trial_id);             // monotonic nonzero
  TEST_ASSERT_NOT_EQUAL(0, a.ack_operation_id);     // quiesce op exposed
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  // Only after PortalQuiesced does the trial begin.
  a = step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));
  TEST_ASSERT_TRUE(a.begin_candidate_trial);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
}

void test_stale_portal_quiesced_ignored() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  PortalAck good = ackOf(s);
  PortalAck wrong = {good.session_id, good.trial_id, good.operation_id + 7};
  step(&s, 1000, PortalInput::PortalQuiesced, wrong);  // wrong operation_id
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(s.state));
  step(&s, 1000, PortalInput::PortalQuiesced, good);   // correct identity
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));
}

// --- candidate submit gating (fix 6) ----------------------------------------

void test_submit_requires_authorized_and_session_bound_artifact() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  // Not authorized -> rejected.
  PortalActions a = submit(&s, 1000, authFor(id, /*ok=*/false));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_FALSE(a.stop_listener);
  TEST_ASSERT_EQUAL_UINT32(0, s.trial_id);
  // Authorized but wrong session -> rejected.
  a = submit(&s, 1000, authFor(id + 1, /*ok=*/true));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_EQUAL_UINT32(0, s.trial_id);
  // Authorized and bound -> accepted.
  a = submit(&s, 1000, authFor(id));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(s.state));
  TEST_ASSERT_TRUE(a.stop_listener);
}

void test_submit_only_from_setup_session() {
  PortalSession s;
  toOnline(&s);
  // StaOnline: no session.
  TEST_ASSERT_FALSE(submit(&s, 0, authFor(1)).stop_listener);
  // PortalRadioPrep.
  step(&s, 0, PortalInput::ConfigureButton);
  TEST_ASSERT_FALSE(submit(&s, 0, authFor(s.session_id)).stop_listener);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  // PortalAwaitSecrets.
  step(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_FALSE(submit(&s, 0, authFor(s.session_id)).stop_listener);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalAwaitSecrets), S(s.state));
}

void test_second_submit_for_active_trial_rejected() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));                 // CandidateQuiesce, trial 1
  const uint32_t trial1 = s.trial_id;
  // A second submission while a trial is active is rejected (not SetupSession).
  PortalActions a = submit(&s, 1000, authFor(id));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(s.state));
  TEST_ASSERT_FALSE(a.stop_listener);
  TEST_ASSERT_EQUAL_UINT32(trial1, s.trial_id);      // unchanged
}

void test_corrected_resubmission_after_failure_uses_new_trial_id() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  const uint32_t deadline0 = s.deadline_start_ms;
  stepSubmit(&s, 1000, authFor(id));
  const uint32_t trial1 = s.trial_id;
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  step(&s, 2000, PortalInput::CandidateFailed, ackOf(s));   // -> ReopenRestore
  step(&s, 2000, PortalInput::RestoreDone, ackOf(s));      // reopen (not expired)
  step(&s, 2000, PortalInput::PortalRadioReady, ackOf(s));  // back to SetupSession
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  // Same session token authorizes a corrected new submission with a new trial id.
  stepSubmit(&s, 3000, authFor(id));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateQuiesce), S(s.state));
  TEST_ASSERT_TRUE(s.trial_id > trial1);
  TEST_ASSERT_EQUAL_UINT32(id, s.session_id);             // same session
  TEST_ASSERT_EQUAL_UINT32(deadline0, s.deadline_start_ms);  // original deadline
}

void test_submit_after_expiry_rejected() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  PortalActions a = submit(&s, 300001, authFor(id));  // past the 5-min deadline
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_FALSE(a.stop_listener);
  TEST_ASSERT_EQUAL_UINT32(0, s.trial_id);
}

// --- candidate success / failure --------------------------------------------

void test_candidate_success_commits_then_online_and_forces_one_fetch() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  PortalActions a = step(&s, 1500, PortalInput::CandidateConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Commit), S(s.state));
  TEST_ASSERT_TRUE(a.commit_candidate_flash);
  a = step(&s, 1600, PortalInput::CommitDone, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_TRUE(a.resume_adsb);
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
  TEST_ASSERT_FALSE(s.secrets_active);
  TEST_ASSERT_TRUE(s.has_old_config);
}

void test_candidate_failure_reopens_same_session_without_resetting_deadline() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);  // opened at t=0
  const uint32_t deadline0 = s.deadline_start_ms;
  stepSubmit(&s, 100000, authFor(id));             // trial at t=100s
  step(&s, 100000, PortalInput::PortalQuiesced, ackOf(s));
  PortalActions a = step(&s, 101000, PortalInput::CandidateFailed, ackOf(s));
  // Step 1: restore the old RAM config FIRST (fresh op), NOT the AP radio, and
  // await RestoreDone. Restore-old and start-AP are never combined.
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
  TEST_ASSERT_TRUE(a.disconnect_sta);
  TEST_ASSERT_FALSE(a.start_ap_radio);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(s.secrets_active);
  TEST_ASSERT_EQUAL_UINT32(id, s.session_id);
  TEST_ASSERT_EQUAL_UINT32(deadline0, s.deadline_start_ms);
  TEST_ASSERT_NOT_EQUAL(0, a.ack_operation_id);    // operation-bound restore
  // Step 2: only RestoreDone starts a new AP-radio preparation (no restore now).
  a = step(&s, 101000, PortalInput::RestoreDone, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_TRUE(a.start_ap_radio);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(s.secrets_active);
  // Radio ready skips secret regen and returns to the listener directly.
  a = step(&s, 101000, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_TRUE(a.start_listener);
  TEST_ASSERT_FALSE(a.request_secret_generation);
  // The original deadline is intact: it still times out at 300s.
  step(&s, 300000, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
}

void test_no_flash_commit_on_failed_candidate_preserves_old_credentials() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  bool any_commit = false;
  any_commit |= stepSubmit(&s, 1000, authFor(id)).commit_candidate_flash;
  any_commit |= step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s))
                    .commit_candidate_flash;
  any_commit |= step(&s, 1500, PortalInput::CandidateFailed, ackOf(s))
                    .commit_candidate_flash;
  TEST_ASSERT_FALSE(any_commit);
  TEST_ASSERT_TRUE(s.has_old_config);
}

// --- deadline enforcement across phases (fix 2) -----------------------------

void test_timeout_boundary_closes_session_with_old_config() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  (void)id;
  upd(&s, 299999, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  PortalActions a = upd(&s, 300000, PortalInput::None);
  // Ordered cleanup: stop the listener first, restore only after the ack.
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_TRUE(a.stop_listener);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(s.secrets_active);
  a = upd(&s, 300000, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

// Each async completion type is preempted by expiry -- not just a Tick. Checked
// at the exact boundary and, separately, across the millis() rollover.
void checkPreemptionBoundary(PortalState target, PortalInput completion,
                             uint32_t open_ms) {
  const uint32_t not_expired = open_ms + 299999u;
  const uint32_t expired = open_ms + 300000u;
  {  // Just before the deadline: processed normally (never a cleanup state).
    PortalSession s;
    driveTo(&s, target, open_ms);
    step(&s, not_expired, completion, ackOf(s));
    TEST_ASSERT_NOT_EQUAL(S(PortalState::SetupCleanup), S(s.state));
    TEST_ASSERT_NOT_EQUAL(S(PortalState::TrialCancel), S(s.state));
  }
  {  // At the deadline: preempted into ordered cleanup; no listener/trial/commit.
    PortalSession s;
    driveTo(&s, target, open_ms);
    PortalActions a = step(&s, expired, completion, ackOf(s));
    const bool cleanup = s.state == PortalState::SetupCleanup ||
                         s.state == PortalState::TrialCancel;
    TEST_ASSERT_TRUE(cleanup);
    TEST_ASSERT_FALSE(a.start_listener);
    TEST_ASSERT_FALSE(a.begin_candidate_trial);
    TEST_ASSERT_FALSE(a.commit_candidate_flash);
    TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  }
}

void test_deadline_boundary_each_completion_type() {
  checkPreemptionBoundary(PortalState::PortalRadioPrep,
                          PortalInput::PortalRadioReady, 0);
  checkPreemptionBoundary(PortalState::PortalAwaitSecrets,
                          PortalInput::SecretsReady, 0);
  checkPreemptionBoundary(PortalState::CandidateQuiesce,
                          PortalInput::PortalQuiesced, 0);
  checkPreemptionBoundary(PortalState::CandidateTrial,
                          PortalInput::CandidateConnected, 0);
  checkPreemptionBoundary(PortalState::CandidateTrial,
                          PortalInput::CandidateFailed, 0);
}

void test_deadline_rollover_each_completion_type() {
  const uint32_t near_wrap = 0xFFFFFF9Cu;  // open_ms + 300000 wraps past 2^32
  checkPreemptionBoundary(PortalState::PortalRadioPrep,
                          PortalInput::PortalRadioReady, near_wrap);
  checkPreemptionBoundary(PortalState::PortalAwaitSecrets,
                          PortalInput::SecretsReady, near_wrap);
  checkPreemptionBoundary(PortalState::CandidateQuiesce,
                          PortalInput::PortalQuiesced, near_wrap);
  checkPreemptionBoundary(PortalState::CandidateTrial,
                          PortalInput::CandidateConnected, near_wrap);
  checkPreemptionBoundary(PortalState::CandidateTrial,
                          PortalInput::CandidateFailed, near_wrap);
}

void test_expired_candidate_connected_never_commits() {
  PortalSession s;
  driveTo(&s, PortalState::CandidateTrial, 0);
  PortalActions a = step(&s, 300000, PortalInput::CandidateConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));  // not Commit
  TEST_ASSERT_FALSE(a.commit_candidate_flash);
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);  // NOT until the cancel is acked
  // The candidate cancel is acknowledged, THEN restore.
  a = step(&s, 300000, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_stalled_trial_tick_cancels_and_restores() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  // No connect/fail arrives. A tick past the deadline must cancel, then restore
  // only after the cancel ack.
  PortalActions a = upd(&s, 300000, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = step(&s, 300000, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_candidate_failure_after_expiry_never_reopens() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  stepSubmit(&s, 100000, authFor(id));
  step(&s, 100000, PortalInput::PortalQuiesced, ackOf(s));
  // Failure arrives AFTER the deadline: must cancel + close, never reopen.
  PortalActions a = step(&s, 301000, PortalInput::CandidateFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = step(&s, 301000, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_timeout_zero_is_disabled() {
  PortalSessionPolicy no_timeout = {0};
  PortalSession s;
  toOnline(&s);
  updP(&s, no_timeout, 0, PortalInput::ConfigureButton);
  updP(&s, no_timeout, 0, PortalInput::PortalRadioReady, ackOf(s));
  updP(&s, no_timeout, 0, PortalInput::SecretsReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  // A tick far in the future must NOT close (0 == unlimited).
  updP(&s, no_timeout, 4000000000u, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
}

// --- commit failure (fix 5) -------------------------------------------------

void test_commit_failure_cancels_candidate_before_old_restore() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  step(&s, 1500, PortalInput::CandidateConnected, ackOf(s));  // Commit
  PortalActions a = step(&s, 1600, PortalInput::CommitFailed, ackOf(s));
  // The candidate may still own a LAN IP: cancel it first, NO restore yet.
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_TRUE(s.has_old_config);   // old flash credentials untouched
  TEST_ASSERT_FALSE(s.secrets_active);
  // Only after the candidate cancel is acknowledged is the old config restored.
  a = step(&s, 1650, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
  TEST_ASSERT_TRUE(s.has_old_config);
}

void test_restore_failure_goes_idle_not_stuck() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  (void)id;
  upd(&s, 5000, PortalInput::SessionCancel);                // -> SetupCleanup
  upd(&s, 5050, PortalInput::PortalQuiesced, ackOf(s));     // -> Restore
  PortalActions a = upd(&s, 5100, PortalInput::RestoreFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));  // not stuck
  // An unsolicited StaConnected in idle is NOT adopted; the adapter must request a
  // new generation (StaRetry) first. The pending fetch survives to that reconnect.
  a = upd(&s, 8000, PortalInput::StaConnected, ackOf(s));  // stale/unsolicited
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));  // ignored
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
  a = upd(&s, 9000, PortalInput::StaRetry);                // mint a new generation
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  a = upd(&s, 9500, PortalInput::StaConnected, ackOf(s));  // now accepted
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
}

// --- ordered cleanup with reordered / delayed acks (fix 3) ------------------

void test_setup_cleanup_waits_for_quiesce_before_restore() {
  PortalSession s;
  openSetupFromOnline(&s);
  step(&s, 1000, PortalInput::SessionCancel);  // SetupCleanup (stop in flight)
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  // A spurious/delayed CandidateCancelled (matching identity) is NOT the awaited
  // ack: it must not settle or restore.
  PortalActions a = step(&s, 1000, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  // A tick likewise does not restore while the stop is unacknowledged.
  a = step(&s, 1100, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  // Only the matching PortalQuiesced settles the close.
  a = step(&s, 1200, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_trial_cancel_waits_for_cancel_before_restore() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  step(&s, 1500, PortalInput::SessionCancel);             // TrialCancel
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  // A delayed PortalQuiesced (matching identity) is NOT the awaited ack here.
  PortalActions a = step(&s, 1500, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  // Only the matching CandidateCancelled settles the close.
  a = step(&s, 1600, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_stale_stop_ack_replayed_is_ignored() {
  PortalSession s;
  openSetupFromOnline(&s);
  step(&s, 1000, PortalInput::SessionCancel);  // SetupCleanup (op = cleanup stop)
  PortalAck good = ackOf(s);
  PortalAck stale = {good.session_id, good.trial_id, good.operation_id - 1};
  PortalActions a = step(&s, 1000, PortalInput::PortalQuiesced, stale);  // old op
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));  // ignored
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = step(&s, 1000, PortalInput::PortalQuiesced, good);  // current op
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
}

void test_stale_restore_ack_replayed_is_ignored() {
  PortalSession s;
  openSetupFromOnline(&s);
  step(&s, 1000, PortalInput::SessionCancel);
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // Restore (op = restore)
  PortalAck good = ackOf(s);
  PortalAck stale = {good.session_id, good.trial_id, good.operation_id - 1};
  step(&s, 1000, PortalInput::RestoreDone, stale);  // old op -> ignored
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  step(&s, 1000, PortalInput::RestoreDone, good);   // current op
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
}

void test_stale_commit_ack_replayed_is_ignored() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  step(&s, 1500, PortalInput::CandidateConnected, ackOf(s));  // Commit (op=commit)
  PortalAck good = ackOf(s);
  PortalAck stale = {good.session_id, good.trial_id, good.operation_id - 1};
  step(&s, 1600, PortalInput::CommitDone, stale);  // old op -> ignored
  TEST_ASSERT_EQUAL_INT(S(PortalState::Commit), S(s.state));
  step(&s, 1600, PortalInput::CommitDone, good);   // current op
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
}

// --- first-boot failure / rollback (fix 4) ----------------------------------

uint32_t openFirstBootSetup(PortalSession* s) {
  portalSessionInit(s);
  step(s, 0, PortalInput::BootNoCredentials);            // PortalRadioPrep
  step(s, 0, PortalInput::PortalRadioReady, ackOf(*s));  // PortalAwaitSecrets
  step(s, 0, PortalInput::SecretsReady, ackOf(*s));      // SetupSession
  return s->session_id;
}

void test_first_boot_candidate_failure_reenters_without_restore() {
  PortalSession s;
  const uint32_t id = openFirstBootSetup(&s);
  TEST_ASSERT_FALSE(s.has_old_config);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  PortalActions a = step(&s, 1500, PortalInput::CandidateFailed, ackOf(s));
  // Re-enter the SAME secured session; NOTHING to restore on first boot.
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_TRUE(a.disconnect_sta);
  TEST_ASSERT_TRUE(a.start_ap_radio);
  TEST_ASSERT_TRUE(s.secrets_active);           // same secrets
  TEST_ASSERT_EQUAL_UINT32(id, s.session_id);   // same session
  // Radio ready returns to the listener directly (secrets still valid).
  a = step(&s, 1500, PortalInput::PortalRadioReady, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_TRUE(a.start_listener);
  TEST_ASSERT_FALSE(a.request_secret_generation);
}

void test_first_boot_candidate_retry_after_failure_succeeds() {
  PortalSession s;
  const uint32_t id = openFirstBootSetup(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  step(&s, 1500, PortalInput::CandidateFailed, ackOf(s));       // reopen
  step(&s, 1500, PortalInput::PortalRadioReady, ackOf(s));      // SetupSession
  // A corrected retry now connects and commits.
  stepSubmit(&s, 2000, authFor(id));
  step(&s, 2000, PortalInput::PortalQuiesced, ackOf(s));
  PortalActions a = step(&s, 2500, PortalInput::CandidateConnected, ackOf(s));
  TEST_ASSERT_TRUE(a.commit_candidate_flash);
  a = step(&s, 2600, PortalInput::CommitDone, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_TRUE(s.has_old_config);  // now there IS a stored config
}

void test_first_boot_failure_then_timeout_goes_idle_after_quiescence() {
  PortalSession s;
  const uint32_t id = openFirstBootSetup(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  // Deadline passes while the trial stalls: cancel, then go idle (no restore).
  PortalActions a = upd(&s, 300000, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = step(&s, 300000, PortalInput::CandidateCancelled, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(s.pending_immediate_fetch);
}

void test_first_boot_cancel_goes_idle_no_restore() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootNoCredentials);  // PortalRadioPrep, no old config
  PortalActions a = upd(&s, 1000, PortalInput::SessionCancel);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_TRUE(a.stop_listener);            // tear down AP radio first
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = upd(&s, 1050, PortalInput::PortalQuiesced, ackOf(s));  // -> idle
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);  // nothing to restore
  TEST_ASSERT_FALSE(a.start_sta_connect);
  TEST_ASSERT_FALSE(s.pending_immediate_fetch);
  // A later background reconnect (StaRetry mints a generation) then a real STA
  // link brings us online with NO forced fetch/resume (nothing was pending).
  a = upd(&s, 4000, PortalInput::StaRetry);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  a = upd(&s, 5000, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
  TEST_ASSERT_FALSE(a.resume_adsb);
}

void test_first_boot_secrets_failure_goes_idle() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootNoCredentials);
  upd(&s, 0, PortalInput::PortalRadioReady, ackOf(s));
  PortalActions a = upd(&s, 0, PortalInput::SecretsFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupCleanup), S(s.state));
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(a.stop_listener);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  a = upd(&s, 0, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
}

// --- cancel / restore / forced fetch (has_old_config) -----------------------

void test_cancel_closes_and_reconnect_forces_exactly_one_fetch() {
  PortalSession s;
  openSetupFromOnline(&s);
  int force_count = 0;
  PortalActions a = upd(&s, 5000, PortalInput::SessionCancel);  // SetupCleanup
  force_count += a.force_immediate_adsb_fetch ? 1 : 0;
  a = upd(&s, 5050, PortalInput::PortalQuiesced, ackOf(s));  // -> Restore
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  force_count += a.force_immediate_adsb_fetch ? 1 : 0;
  a = upd(&s, 5100, PortalInput::RestoreDone, ackOf(s));  // -> StaConnecting
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  force_count += a.force_immediate_adsb_fetch ? 1 : 0;
  a = upd(&s, 6000, PortalInput::StaConnected, ackOf(s));  // -> StaOnline
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  force_count += a.force_immediate_adsb_fetch ? 1 : 0;
  TEST_ASSERT_TRUE(a.resume_adsb);
  TEST_ASSERT_EQUAL_INT(1, force_count);
}

void test_forced_fetch_survives_a_failed_reconnect_attempt() {
  PortalSession s;
  openSetupFromOnline(&s);
  upd(&s, 5000, PortalInput::SessionCancel);
  upd(&s, 5050, PortalInput::PortalQuiesced, ackOf(s));
  upd(&s, 5100, PortalInput::RestoreDone, ackOf(s));
  PortalActions a = upd(&s, 6000, PortalInput::StaConnectFailed, ackOf(s));  // OfflineIdle
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.force_immediate_adsb_fetch);
  // The pending fetch survives the failed attempt: a fresh generation (StaRetry)
  // then a real connect still forces exactly one immediate fetch.
  upd(&s, 19000, PortalInput::StaRetry);                 // -> StaConnecting
  a = upd(&s, 20000, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
}

// --- erase is confirmation-only (fix 9) -------------------------------------

void test_erase_only_via_confirmed_input() {
  PortalSession s;
  toOnline(&s);
  PortalActions a = step(&s, 0, PortalInput::EraseConfirmed);
  TEST_ASSERT_EQUAL_INT(S(PortalState::FactoryErase), S(s.state));
  TEST_ASSERT_TRUE(a.factory_erase);
  a = step(&s, 0, PortalInput::EraseDone);
  TEST_ASSERT_EQUAL_INT(S(PortalState::BootDecide), S(s.state));
  TEST_ASSERT_FALSE(s.has_old_config);
}

void test_erase_from_setup_session_stops_listener_and_zeroizes() {
  PortalSession s;
  openSetupFromOnline(&s);
  PortalActions a = step(&s, 0, PortalInput::EraseConfirmed);
  TEST_ASSERT_EQUAL_INT(S(PortalState::FactoryErase), S(s.state));
  TEST_ASSERT_TRUE(a.stop_listener);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_TRUE(a.factory_erase);
}

void test_erase_from_candidate_trial_cancels_candidate() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  PortalActions a = step(&s, 1500, PortalInput::EraseConfirmed);
  TEST_ASSERT_EQUAL_INT(S(PortalState::FactoryErase), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_TRUE(a.factory_erase);
}

void test_erase_from_expired_session_still_allowed() {
  // Expiry preempts non-destructive events, but EraseConfirmed still takes the
  // physical destructive path.
  PortalSession s;
  openSetupFromOnline(&s, 0);
  PortalActions a = step(&s, 301000, PortalInput::EraseConfirmed);
  TEST_ASSERT_EQUAL_INT(S(PortalState::FactoryErase), S(s.state));
  TEST_ASSERT_TRUE(a.factory_erase);
}

void test_no_submit_or_other_input_ever_triggers_erase() {
  const PortalInput non_erase[] = {
      PortalInput::None,           PortalInput::PortalRadioReady,
      PortalInput::SecretsReady,   PortalInput::PortalQuiesced,
      PortalInput::CandidateConnected, PortalInput::CandidateFailed,
      PortalInput::CandidateCancelled, PortalInput::SessionCancel,
      PortalInput::CommitDone,     PortalInput::CommitFailed,
      PortalInput::RestoreDone};
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  for (int i = 0; i < 30; ++i) {
    for (PortalInput in : non_erase) {
      PortalActions a = upd(&s, static_cast<uint32_t>(1000 + i), in, ackOf(s));
      TEST_ASSERT_FALSE(a.factory_erase);
    }
    // Submissions likewise never erase.
    TEST_ASSERT_FALSE(submit(&s, 1000, authFor(id)).factory_erase);
  }
}

// --- idempotency ------------------------------------------------------------

void test_cancel_when_not_in_session_is_noop() {
  PortalSession s;
  toOnline(&s);
  PortalActions a = upd(&s, 0, PortalInput::SessionCancel);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_FALSE(a.stop_listener);
  TEST_ASSERT_FALSE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(a.zeroize_secrets);
}

void test_repeated_configure_in_session_is_idempotent() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  PortalActions a = upd(&s, 1000, PortalInput::ConfigureButton);
  TEST_ASSERT_EQUAL_INT(S(PortalState::SetupSession), S(s.state));
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_EQUAL_UINT32(id, s.session_id);
}

void test_sta_lost_from_online_reconnects() {
  PortalSession s;
  toOnline(&s);
  PortalActions a = upd(&s, 0, PortalInput::StaLost, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  TEST_ASSERT_TRUE(a.start_sta_connect);
}

// --- reopen-restore before same-session reopen (fix: await old-config restore) --

void test_reopen_restore_failed_fails_closed_to_idle() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));    // CandidateTrial
  PortalActions a = step(&s, 2000, PortalInput::CandidateFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  TEST_ASSERT_TRUE(a.restore_old_ram_config);
  TEST_ASSERT_FALSE(a.start_ap_radio);
  // The old-config restore fails: fail closed. Zeroize, no listener, go idle.
  a = step(&s, 2100, PortalInput::RestoreFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(s.secrets_active);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_FALSE(a.start_ap_radio);
  TEST_ASSERT_FALSE(core::portalListenerActive(s.state));
  // A later reconnect (fresh generation) still forces one immediate fetch.
  step(&s, 3000, PortalInput::StaRetry);
  a = step(&s, 3500, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
}

void test_reopen_restore_expiry_reconnects_never_reopens() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);  // opened at t=0, deadline 300s
  stepSubmit(&s, 100000, authFor(id));
  step(&s, 100000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  PortalActions a = step(&s, 101000, PortalInput::CandidateFailed, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));  // restoring
  // The deadline lapses WHILE restoration is in flight. A plain tick does not
  // preempt a restore-in-flight (it must run to completion).
  a = step(&s, 300001, PortalInput::None);
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  // RestoreDone past the deadline must settle the old STA, NOT reopen the portal,
  // and must never start a listener: zeroize secrets, reconnect, arm one fetch.
  a = step(&s, 300002, PortalInput::RestoreDone, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  TEST_ASSERT_TRUE(a.start_sta_connect);
  TEST_ASSERT_FALSE(a.start_ap_radio);
  TEST_ASSERT_FALSE(a.start_listener);
  TEST_ASSERT_TRUE(a.zeroize_secrets);
  TEST_ASSERT_FALSE(s.secrets_active);        // NOT preserved past expiry
  a = step(&s, 301000, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  TEST_ASSERT_TRUE(a.force_immediate_adsb_fetch);
}

void test_reopen_restore_stale_ack_and_no_radio_ready_standin() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s, 0);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  step(&s, 2000, PortalInput::CandidateFailed, ackOf(s));   // -> ReopenRestore
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  PortalAck good = ackOf(s);
  // A PortalRadioReady must NOT stand in for RestoreDone (even matching op).
  PortalActions a = step(&s, 2000, PortalInput::PortalRadioReady, good);
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));
  TEST_ASSERT_FALSE(a.start_ap_radio);
  // A stale RestoreDone from an earlier operation is rejected.
  PortalAck stale = {good.session_id, good.trial_id, good.operation_id - 1,
                     good.sta_connection_id};
  a = step(&s, 2000, PortalInput::RestoreDone, stale);
  TEST_ASSERT_EQUAL_INT(S(PortalState::ReopenRestore), S(s.state));  // ignored
  // Only the current RestoreDone reopens the AP-radio preparation.
  a = step(&s, 2000, PortalInput::RestoreDone, good);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
  TEST_ASSERT_TRUE(a.start_ap_radio);
  // A stale RestoreDone replayed AFTER moving on is likewise rejected.
  a = step(&s, 2000, PortalInput::RestoreDone, good);
  TEST_ASSERT_EQUAL_INT(S(PortalState::PortalRadioPrep), S(s.state));
}

// --- normal-STA link generation binding (fix: generation-bound STA events) ---

void test_stale_boot_connect_success_rejected_during_later_attempt() {
  PortalSession s;
  portalSessionInit(&s);
  PortalActions a = upd(&s, 0, PortalInput::BootHasCredentials);  // StaConnecting
  const uint32_t sc1 = s.sta_connection_id;
  TEST_ASSERT_NOT_EQUAL(0, sc1);
  TEST_ASSERT_EQUAL_UINT32(sc1, a.ack_sta_connection_id);
  upd(&s, 1000, PortalInput::StaConnectFailed, ackOf(s));         // StaOfflineIdle
  a = upd(&s, 2000, PortalInput::StaRetry);                       // StaConnecting
  const uint32_t sc2 = s.sta_connection_id;
  TEST_ASSERT_NOT_EQUAL(sc1, sc2);
  // A delayed StaConnected from the FIRST attempt must be rejected.
  PortalAck old = {0, 0, 0, sc1};
  upd(&s, 2100, PortalInput::StaConnected, old);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));  // ignored
  // The current attempt's success is adopted.
  upd(&s, 2200, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
}

void test_stale_boot_connect_failure_rejected_during_later_attempt() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootHasCredentials);                    // StaConnecting
  const uint32_t sc1 = s.sta_connection_id;
  upd(&s, 1000, PortalInput::StaConnectFailed, ackOf(s));         // idle
  upd(&s, 2000, PortalInput::StaRetry);                           // StaConnecting
  const uint32_t sc2 = s.sta_connection_id;
  TEST_ASSERT_NOT_EQUAL(sc1, sc2);
  // A delayed FAILURE from the first attempt must not abort the second.
  PortalAck old = {0, 0, 0, sc1};
  upd(&s, 2100, PortalInput::StaConnectFailed, old);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));  // ignored
  upd(&s, 2200, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
}

void test_stale_sta_lost_after_reconnect_rejected() {
  PortalSession s;
  toOnline(&s);                                    // active STA generation = g1
  const uint32_t g1 = s.sta_connection_id;
  PortalActions a = upd(&s, 1000, PortalInput::StaLost, ackOf(s));  // -> reconnect
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  const uint32_t g2 = s.sta_connection_id;
  TEST_ASSERT_NOT_EQUAL(g1, g2);
  upd(&s, 1100, PortalInput::StaConnected, ackOf(s));  // StaOnline on g2
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  // A delayed StaLost carrying the OLD generation must be ignored.
  PortalAck old = {0, 0, 0, g1};
  upd(&s, 1200, PortalInput::StaLost, old);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));  // stays online
  // The active generation's StaLost is honored.
  a = upd(&s, 1300, PortalInput::StaLost, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  TEST_ASSERT_TRUE(a.start_sta_connect);
}

void test_candidate_completion_matches_sta_connection_id() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  PortalActions a = step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));
  TEST_ASSERT_TRUE(a.begin_candidate_trial);
  TEST_ASSERT_NOT_EQUAL(0, a.ack_sta_connection_id);
  const uint32_t cand = s.sta_connection_id;
  // A candidate completion carrying the wrong STA generation is rejected, even
  // with the correct session/trial/operation ids.
  PortalAck wrong_sta = {s.session_id, s.trial_id, s.operation_id, cand + 777};
  a = step(&s, 1500, PortalInput::CandidateConnected, wrong_sta);
  TEST_ASSERT_EQUAL_INT(S(PortalState::CandidateTrial), S(s.state));  // ignored
  TEST_ASSERT_FALSE(a.commit_candidate_flash);
  // The full identity (including the trial's STA generation) commits.
  a = step(&s, 1500, PortalInput::CandidateConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::Commit), S(s.state));
  TEST_ASSERT_TRUE(a.commit_candidate_flash);
}

void test_trial_cancel_ack_carries_candidate_sta_generation() {
  // A cancel completion is modeled by the identity the CANCEL ACTION exposed (as
  // a real adapter echoes it), proving CandidateCancelled binds the candidate's
  // STA generation -- not just session/trial/operation.
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));  // CandidateTrial
  const uint32_t cand_gen = s.sta_connection_id;
  PortalActions a = step(&s, 1500, PortalInput::SessionCancel);  // TrialCancel
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));
  TEST_ASSERT_TRUE(a.cancel_candidate_trial);
  TEST_ASSERT_EQUAL_UINT32(cand_gen, a.ack_sta_connection_id);  // exposed to echo
  // Reconstruct the completion from the action's exposed identity (adapter echo).
  PortalAck echo = {a.ack_session_id, a.ack_trial_id, a.ack_operation_id,
                    a.ack_sta_connection_id};
  // A cancel-ack with a stale STA generation is rejected.
  PortalAck wrong = {echo.session_id, echo.trial_id, echo.operation_id,
                     echo.sta_connection_id + 51};
  PortalActions b = step(&s, 1600, PortalInput::CandidateCancelled, wrong);
  TEST_ASSERT_EQUAL_INT(S(PortalState::TrialCancel), S(s.state));  // ignored
  TEST_ASSERT_FALSE(b.restore_old_ram_config);
  // The action-exposed identity settles the cancel and restores.
  b = step(&s, 1600, PortalInput::CandidateCancelled, echo);
  TEST_ASSERT_EQUAL_INT(S(PortalState::Restore), S(s.state));
  TEST_ASSERT_TRUE(b.restore_old_ram_config);
}

void test_commit_adopts_candidate_connection_generation() {
  PortalSession s;
  const uint32_t id = openSetupFromOnline(&s);
  const uint32_t boot_gen = s.sta_connection_id;   // pre-portal STA generation
  stepSubmit(&s, 1000, authFor(id));
  step(&s, 1000, PortalInput::PortalQuiesced, ackOf(s));      // CandidateTrial
  const uint32_t cand_gen = s.sta_connection_id;
  TEST_ASSERT_NOT_EQUAL(boot_gen, cand_gen);
  step(&s, 1500, PortalInput::CandidateConnected, ackOf(s));  // Commit
  PortalActions a = step(&s, 1600, PortalInput::CommitDone, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
  // The candidate's generation is now the active normal-STA id.
  TEST_ASSERT_EQUAL_UINT32(cand_gen, s.sta_connection_id);
  // A StaLost carrying the stale pre-portal generation is rejected.
  PortalAck old = {0, 0, 0, boot_gen};
  a = upd(&s, 1700, PortalInput::StaLost, old);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));  // ignored
  // A StaLost on the adopted candidate generation is honored.
  a = upd(&s, 1800, PortalInput::StaLost, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
}

void test_offline_idle_ignores_unsolicited_sta_connected() {
  PortalSession s;
  portalSessionInit(&s);
  upd(&s, 0, PortalInput::BootHasCredentials);           // StaConnecting (gen 1)
  upd(&s, 100, PortalInput::StaConnectFailed, ackOf(s));  // StaOfflineIdle
  // An unsolicited StaConnected (even echoing the last generation) is NOT
  // adopted in idle: there is no outstanding attempt until a retry mints one.
  PortalActions a = upd(&s, 1000, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOfflineIdle), S(s.state));
  TEST_ASSERT_FALSE(a.resume_adsb);
  // After StaRetry mints a fresh generation, a matching StaConnected is adopted.
  a = upd(&s, 2000, PortalInput::StaRetry);
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaConnecting), S(s.state));
  TEST_ASSERT_TRUE(a.start_sta_connect);
  upd(&s, 2100, PortalInput::StaConnected, ackOf(s));
  TEST_ASSERT_EQUAL_INT(S(PortalState::StaOnline), S(s.state));
}

// --- global invariant sweeps (fix 9) ----------------------------------------

void test_listener_active_only_in_setup_session() {
  const PortalState all[] = {
      PortalState::BootDecide,        PortalState::StaConnecting,
      PortalState::StaOnline,         PortalState::StaOfflineIdle,
      PortalState::PortalRadioPrep,   PortalState::PortalAwaitSecrets,
      PortalState::SetupSession,      PortalState::CandidateQuiesce,
      PortalState::CandidateTrial,    PortalState::ReopenRestore,
      PortalState::Commit,            PortalState::SetupCleanup,
      PortalState::TrialCancel,       PortalState::Restore,
      PortalState::FactoryErase};
  for (PortalState st : all) {
    const bool expect = (st == PortalState::SetupSession);
    TEST_ASSERT_EQUAL_INT(expect ? 1 : 0,
                          core::portalListenerActive(st) ? 1 : 0);
  }
}

void test_adsb_active_only_in_sta_online() {
  const PortalState all[] = {
      PortalState::BootDecide,        PortalState::StaConnecting,
      PortalState::StaOnline,         PortalState::StaOfflineIdle,
      PortalState::PortalRadioPrep,   PortalState::PortalAwaitSecrets,
      PortalState::SetupSession,      PortalState::CandidateQuiesce,
      PortalState::CandidateTrial,    PortalState::ReopenRestore,
      PortalState::Commit,            PortalState::SetupCleanup,
      PortalState::TrialCancel,       PortalState::Restore,
      PortalState::FactoryErase};
  for (PortalState st : all) {
    const bool expect = (st == PortalState::StaOnline);
    TEST_ASSERT_EQUAL_INT(expect ? 1 : 0, core::portalAdsbActive(st) ? 1 : 0);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_no_credentials_starts_radio_prep_not_listener);
  RUN_TEST(test_boot_with_credentials_connects_without_listener);
  RUN_TEST(test_boot_connect_success_no_forced_fetch);
  RUN_TEST(test_failed_saved_wifi_goes_offline_idle_never_opens_listener);
  RUN_TEST(test_setup_open_sequences_radio_then_secrets_then_listener);
  RUN_TEST(test_secrets_failure_fails_closed_without_listener);
  RUN_TEST(test_secrets_not_active_before_acknowledgement);
  RUN_TEST(test_stale_radio_ready_is_ignored);
  RUN_TEST(test_stale_secrets_ready_is_ignored);
  RUN_TEST(test_stale_first_radio_ready_replayed_on_reopen_is_ignored);
  RUN_TEST(test_stale_completion_from_prior_trial_no_commit_or_reopen);
  RUN_TEST(test_stale_completion_from_prior_session_ignored);
  RUN_TEST(test_candidate_submit_quiesces_before_trial);
  RUN_TEST(test_stale_portal_quiesced_ignored);
  RUN_TEST(test_submit_requires_authorized_and_session_bound_artifact);
  RUN_TEST(test_submit_only_from_setup_session);
  RUN_TEST(test_second_submit_for_active_trial_rejected);
  RUN_TEST(test_corrected_resubmission_after_failure_uses_new_trial_id);
  RUN_TEST(test_submit_after_expiry_rejected);
  RUN_TEST(test_candidate_success_commits_then_online_and_forces_one_fetch);
  RUN_TEST(test_candidate_failure_reopens_same_session_without_resetting_deadline);
  RUN_TEST(test_no_flash_commit_on_failed_candidate_preserves_old_credentials);
  RUN_TEST(test_timeout_boundary_closes_session_with_old_config);
  RUN_TEST(test_deadline_boundary_each_completion_type);
  RUN_TEST(test_deadline_rollover_each_completion_type);
  RUN_TEST(test_expired_candidate_connected_never_commits);
  RUN_TEST(test_stalled_trial_tick_cancels_and_restores);
  RUN_TEST(test_candidate_failure_after_expiry_never_reopens);
  RUN_TEST(test_timeout_zero_is_disabled);
  RUN_TEST(test_commit_failure_cancels_candidate_before_old_restore);
  RUN_TEST(test_restore_failure_goes_idle_not_stuck);
  RUN_TEST(test_setup_cleanup_waits_for_quiesce_before_restore);
  RUN_TEST(test_trial_cancel_waits_for_cancel_before_restore);
  RUN_TEST(test_stale_stop_ack_replayed_is_ignored);
  RUN_TEST(test_stale_restore_ack_replayed_is_ignored);
  RUN_TEST(test_stale_commit_ack_replayed_is_ignored);
  RUN_TEST(test_first_boot_candidate_failure_reenters_without_restore);
  RUN_TEST(test_first_boot_candidate_retry_after_failure_succeeds);
  RUN_TEST(test_first_boot_failure_then_timeout_goes_idle_after_quiescence);
  RUN_TEST(test_first_boot_cancel_goes_idle_no_restore);
  RUN_TEST(test_first_boot_secrets_failure_goes_idle);
  RUN_TEST(test_cancel_closes_and_reconnect_forces_exactly_one_fetch);
  RUN_TEST(test_forced_fetch_survives_a_failed_reconnect_attempt);
  RUN_TEST(test_erase_only_via_confirmed_input);
  RUN_TEST(test_erase_from_setup_session_stops_listener_and_zeroizes);
  RUN_TEST(test_erase_from_candidate_trial_cancels_candidate);
  RUN_TEST(test_erase_from_expired_session_still_allowed);
  RUN_TEST(test_no_submit_or_other_input_ever_triggers_erase);
  RUN_TEST(test_cancel_when_not_in_session_is_noop);
  RUN_TEST(test_repeated_configure_in_session_is_idempotent);
  RUN_TEST(test_sta_lost_from_online_reconnects);
  RUN_TEST(test_reopen_restore_failed_fails_closed_to_idle);
  RUN_TEST(test_reopen_restore_expiry_reconnects_never_reopens);
  RUN_TEST(test_reopen_restore_stale_ack_and_no_radio_ready_standin);
  RUN_TEST(test_stale_boot_connect_success_rejected_during_later_attempt);
  RUN_TEST(test_stale_boot_connect_failure_rejected_during_later_attempt);
  RUN_TEST(test_stale_sta_lost_after_reconnect_rejected);
  RUN_TEST(test_candidate_completion_matches_sta_connection_id);
  RUN_TEST(test_trial_cancel_ack_carries_candidate_sta_generation);
  RUN_TEST(test_commit_adopts_candidate_connection_generation);
  RUN_TEST(test_offline_idle_ignores_unsolicited_sta_connected);
  RUN_TEST(test_listener_active_only_in_setup_session);
  RUN_TEST(test_adsb_active_only_in_sta_online);
  return UNITY_END();
}
