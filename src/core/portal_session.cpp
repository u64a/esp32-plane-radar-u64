#include "core/portal_session.h"

#include "core/time_math.h"

namespace core {

namespace {

// A session's liveness deadline. timeout_ms == 0 means disabled/unlimited (the
// firmware config convention, e.g. config::kWifiPortalTimeoutSec == 0). The
// boundary is inclusive: expired once (now - start) >= timeout, and the unsigned
// subtraction wraps correctly across the millis() rollover.
bool deadlineExpired(const PortalSessionPolicy& policy, uint32_t now_ms,
                     uint32_t start_ms) {
  return policy.session_timeout_ms != 0U &&
         elapsedAtLeast(now_ms, start_ms, policy.session_timeout_ms);
}

// The setup-lifetime states that carry the session liveness deadline and whose
// pending success/failure events are preempted by expiry. Commit, the cleanup
// states, and Restore are deliberately NOT here: once a candidate has connected
// or an ordered rollback is under way, the operation must run to completion.
bool isDeadlineState(PortalState state) {
  switch (state) {
    case PortalState::PortalRadioPrep:
    case PortalState::PortalAwaitSecrets:
    case PortalState::SetupSession:
    case PortalState::CandidateQuiesce:
    case PortalState::CandidateTrial:
      return true;
    default:
      return false;
  }
}

// Advance the per-operation generation. Monotonic and nonzero, so a completion
// carrying a stale operation_id (from any earlier operation, even one of the same
// session/trial) can never match the current outstanding operation.
void nextOperationId(PortalSession* s) {
  s->operation_id += 1U;
  if (s->operation_id == 0U) {
    s->operation_id = 1U;  // skip zero across the (astronomical) wrap
  }
}

// Advance the STA link generation. Monotonic and nonzero (never reused), so a
// delayed StaConnected/StaConnectFailed/StaLost from an earlier attempt cannot
// match a later one. Separate from operation_id: STA link events are not portal
// operations, and a candidate connection generation outlives the trial operation
// that minted it (it becomes the active normal-STA id on commit).
void nextStaConnectionId(PortalSession* s) {
  s->sta_connection_id += 1U;
  if (s->sta_connection_id == 0U) {
    s->sta_connection_id = 1U;  // skip zero across the (astronomical) wrap
  }
}

// Begin a stored/restored STA connect: request it and expose a fresh link
// generation the adapter must echo on StaConnected/StaConnectFailed/StaLost.
void startStaConnect(PortalSession* s, PortalActions* a) {
  a->start_sta_connect = true;
  nextStaConnectionId(s);
  a->ack_sta_connection_id = s->sta_connection_id;
}

// Stamp the actions with the identity the adapter must echo on the completion.
void stampAck(const PortalSession* s, PortalActions* a) {
  a->ack_session_id = s->session_id;
  a->ack_trial_id = s->trial_id;
  a->ack_operation_id = s->operation_id;
}

// Begin a new ack-expecting async operation: mint a fresh operation_id and expose
// the identity for the adapter to echo.
void startOp(PortalSession* s, PortalActions* a) {
  nextOperationId(s);
  stampAck(s, a);
}

// After a listener stop (PortalQuiesced) or candidate cancel (CandidateCancelled)
// has been acknowledged, settle the close: restore the old STA path if a stored
// config exists, otherwise go straight to OfflineIdle. Restore/reconnect is
// emitted ONLY here -- never concurrently with the still-unacknowledged stop or
// cancel that preceded it.
void settleAfterCleanup(PortalSession* s, PortalActions* a) {
  if (s->has_old_config) {
    a->restore_old_ram_config = true;
    s->pending_immediate_fetch = true;
    s->state = PortalState::Restore;
    startOp(s, a);  // restore op; ack = RestoreDone / RestoreFailed
  } else {
    s->pending_immediate_fetch = false;
    s->state = PortalState::StaOfflineIdle;
  }
}

// Begin an ordered close of an active setup session (timeout / cancel / entropy
// failure / commit failure). Zeroizes secrets, then EITHER stops the listener/AP
// radio (SetupCleanup, ack PortalQuiesced) or cancels an in-flight/connected
// candidate (TrialCancel, ack CandidateCancelled). The restore/idle decision is
// deferred to settleAfterCleanup once that stop/cancel is acknowledged, so no
// reconnect or old-config restore is ever emitted while the portal or candidate
// might still be up.
void beginClose(PortalSession* s, PortalActions* a) {
  a->zeroize_secrets = true;
  s->secrets_active = false;
  s->has_candidate = false;
  switch (s->state) {
    case PortalState::PortalRadioPrep:
    case PortalState::PortalAwaitSecrets:
    case PortalState::SetupSession:
    case PortalState::CandidateQuiesce:
      // Listener/AP radio is (being) exposed: (re)issue the stop and wait for the
      // quiesce ack before any restore.
      a->stop_listener = true;
      s->state = PortalState::SetupCleanup;
      startOp(s, a);  // quiesce op; ack = PortalQuiesced
      break;
    case PortalState::CandidateTrial:
    case PortalState::Commit:
      // A candidate connect is in flight or connected (may own a LAN IP): cancel
      // it and wait for the cancel ack before any restore. Expose the candidate's
      // STA generation so the CandidateCancelled completion echoes it (a stale
      // cancel-ack from an earlier candidate connection can never settle this one).
      a->cancel_candidate_trial = true;
      a->ack_sta_connection_id = s->sta_connection_id;
      s->state = PortalState::TrialCancel;
      startOp(s, a);  // cancel op; ack = CandidateCancelled (+ STA generation)
      break;
    default:
      break;
  }
}

// Begin bringing up the portal radio for a brand-new session: bump the id, anchor
// the deadline, and request the pause/disconnect/AP-init sequence. Secrets are
// deliberately NOT marked active here -- only once SecretsReady is acknowledged.
void openNewSession(PortalSession* s, uint32_t now_ms, PortalActions* a) {
  s->session_id += 1U;
  if (s->session_id == 0U) {
    s->session_id = 1U;  // stay nonzero across the (astronomical) wrap
  }
  s->deadline_start_ms = now_ms;
  s->secrets_active = false;
  s->has_candidate = false;
  s->pending_immediate_fetch = false;
  s->state = PortalState::PortalRadioPrep;
  a->pause_adsb = true;       // retains snapshot AND backoff (adapter contract)
  a->disconnect_sta = true;   // drop any STA IP before the AP radio comes up
  a->start_ap_radio = true;   // switch to AP mode / init AP netif (no listener)
  startOp(s, a);              // radio-prep op; ack = PortalRadioReady
}

// Re-open the SAME session after a failed candidate: keep id/secrets/deadline.
// Restoring the old RAM config and reopening the AP radio are now SEPARATE,
// operation-bound steps so the pure protocol can tell restoration apart from
// readiness. With a stored config, first disconnect the failed candidate and
// restore the old RAM config (ReopenRestore, a fresh operation), and await
// RestoreDone before any AP-radio reopen -- an action set NEVER combines
// restore-old with start-AP, and no PortalRadioReady may stand in for RestoreDone.
// On FIRST boot there is nothing to restore, so bring the AP radio straight back.
// Because secrets_active stays true, the eventual PortalRadioReady skips
// regeneration and goes straight to SetupSession. A fresh operation_id is minted
// either way, so a delayed completion from the FIRST preparation can no longer
// match and prematurely advance this new teardown.
void reopenSession(PortalSession* s, PortalActions* a) {
  s->has_candidate = false;
  a->disconnect_sta = true;  // drop the failed candidate STA
  if (s->has_old_config) {
    a->restore_old_ram_config = true;  // old creds become the active RAM config
    s->state = PortalState::ReopenRestore;
    startOp(s, a);  // restore op; ack = RestoreDone / RestoreFailed
  } else {
    a->start_ap_radio = true;
    s->state = PortalState::PortalRadioPrep;
    startOp(s, a);  // NEW radio-prep op (stale-ready guard)
  }
}

// Enter FactoryErase from a confirmed erase, tearing down whatever is live first.
// Erase is the physical destructive path: it does not wait for a quiesce/cancel
// ack.
void enterFactoryErase(PortalSession* s, PortalActions* a) {
  switch (s->state) {
    case PortalState::PortalRadioPrep:
    case PortalState::PortalAwaitSecrets:
    case PortalState::SetupSession:
    case PortalState::CandidateQuiesce:
    case PortalState::SetupCleanup:
      a->stop_listener = true;
      break;
    case PortalState::CandidateTrial:
    case PortalState::Commit:
    case PortalState::TrialCancel:
      a->cancel_candidate_trial = true;
      break;
    default:
      break;
  }
  if (s->secrets_active) {
    a->zeroize_secrets = true;
    s->secrets_active = false;
  }
  a->factory_erase = true;
  s->has_candidate = false;
  s->state = PortalState::FactoryErase;
}

// Enter StaOnline, honoring a pending one-shot fetch armed by a session close.
void enterStaOnline(PortalSession* s, PortalActions* a) {
  s->state = PortalState::StaOnline;
  if (s->pending_immediate_fetch) {
    a->resume_adsb = true;
    a->force_immediate_adsb_fetch = true;
    s->pending_immediate_fetch = false;
  }
}

// Identity gate for delayed async completions. Returns true when `input` is not an
// identity-bound completion, or when the carried ack matches the current
// operation. Session-scoped inputs must match session_id + operation_id; trial-
// scoped candidate-link inputs must additionally match trial_id AND the candidate
// trial's sta_connection_id; flash inputs match session_id + trial_id +
// operation_id; STA link events match ONLY the current sta_connection_id (they are
// not portal operations). Because these generations advance on every new
// operation/attempt, a stale completion is rejected even when its session_id (and
// trial_id) still match the current ones -- e.g. a duplicate PortalRadioReady from
// the first preparation replayed during a same-session reopen, or a delayed
// StaConnected from an earlier connect attempt replayed during a later one.
bool eventIdMatches(const PortalSession* s, PortalInput input, PortalAck ack) {
  switch (input) {
    case PortalInput::PortalRadioReady:
    case PortalInput::SecretsReady:
    case PortalInput::SecretsFailed:
    case PortalInput::PortalQuiesced:
    case PortalInput::RestoreDone:
    case PortalInput::RestoreFailed:
      return ack.session_id == s->session_id &&
             ack.operation_id == s->operation_id;
    case PortalInput::CandidateConnected:
    case PortalInput::CandidateFailed:
    case PortalInput::CandidateCancelled:
      // Candidate link events additionally echo the trial's STA generation.
      return ack.session_id == s->session_id &&
             ack.trial_id == s->trial_id &&
             ack.operation_id == s->operation_id &&
             ack.sta_connection_id == s->sta_connection_id;
    case PortalInput::CommitDone:
    case PortalInput::CommitFailed:
      return ack.session_id == s->session_id &&
             ack.trial_id == s->trial_id &&
             ack.operation_id == s->operation_id;
    case PortalInput::StaConnected:
    case PortalInput::StaConnectFailed:
    case PortalInput::StaLost:
      // STA link events are generation-bound only: a nonzero id must be
      // outstanding AND match, so an unsolicited/stale link event is rejected.
      return s->sta_connection_id != 0U &&
             ack.sta_connection_id == s->sta_connection_id;
    default:
      return true;  // synchronous inputs are not identity-bound
  }
}

}  // namespace

void portalSessionInit(PortalSession* s) {
  if (s == nullptr) {
    return;
  }
  s->state = PortalState::BootDecide;
  s->session_id = 0;
  s->trial_id = 0;
  s->operation_id = 0;
  s->sta_connection_id = 0;
  s->deadline_start_ms = 0;
  s->has_old_config = false;
  s->has_candidate = false;
  s->secrets_active = false;
  s->pending_immediate_fetch = false;
}

PortalActions portalSessionUpdate(PortalSession* s,
                                  const PortalSessionPolicy& policy,
                                  uint32_t now_ms, PortalInput input,
                                  PortalAck ack) {
  PortalActions a = {};
  if (s == nullptr) {
    return a;
  }

  // Reject delayed/stale identity-bound completions from a prior operation (of
  // this or any earlier session/trial).
  if (!eventIdMatches(s, input, ack)) {
    return a;
  }

  // Deadline preemption: in any setup-lifetime state, an expired deadline
  // preempts EVERY non-destructive success/failure event and a plain tick alike,
  // driving an ordered close instead of starting/reopening a listener, starting a
  // candidate, or committing one. EraseConfirmed still takes the physical
  // destructive path.
  if (input != PortalInput::EraseConfirmed && isDeadlineState(s->state) &&
      deadlineExpired(policy, now_ms, s->deadline_start_ms)) {
    beginClose(s, &a);
    return a;
  }

  switch (s->state) {
    case PortalState::BootDecide:
      if (input == PortalInput::BootHasCredentials) {
        s->has_old_config = true;
        s->state = PortalState::StaConnecting;
        startStaConnect(s, &a);  // boot connect with stored creds (mints STA id)
      } else if (input == PortalInput::BootNoCredentials) {
        s->has_old_config = false;
        openNewSession(s, now_ms, &a);  // first boot may open secured setup
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::StaConnecting:
      if (input == PortalInput::StaConnected) {
        enterStaOnline(s, &a);
      } else if (input == PortalInput::StaConnectFailed) {
        s->state = PortalState::StaOfflineIdle;  // never auto-open the portal
      } else if (input == PortalInput::ConfigureButton) {
        openNewSession(s, now_ms, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::StaOnline:
      if (input == PortalInput::StaLost) {
        s->state = PortalState::StaConnecting;
        startStaConnect(s, &a);  // reconnect (mints a fresh STA id)
      } else if (input == PortalInput::ConfigureButton) {
        openNewSession(s, now_ms, &a);  // pause ADS-B, retain snapshot/backoff
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::StaOfflineIdle:
      if (input == PortalInput::ConfigureButton) {
        openNewSession(s, now_ms, &a);
      } else if (input == PortalInput::StaRetry) {
        // Adapter-initiated background reconnect: mint a fresh generation and try
        // again. Only after this can a StaConnected be accepted -- an unsolicited
        // StaConnected in idle is NOT adopted (no outstanding generation).
        s->state = PortalState::StaConnecting;
        startStaConnect(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::PortalRadioPrep:
      // Waiting for the adapter to confirm STA has no IP and the AP radio is up.
      if (input == PortalInput::PortalRadioReady) {
        if (s->secrets_active) {
          // Reopen path: secrets are still valid, so go straight to the listener.
          s->state = PortalState::SetupSession;
          a.start_listener = true;
        } else {
          s->state = PortalState::PortalAwaitSecrets;
          a.request_secret_generation = true;
          startOp(s, &a);  // secrets op; ack = SecretsReady / SecretsFailed
        }
      } else if (input == PortalInput::SessionCancel) {
        beginClose(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::PortalAwaitSecrets:
      // Waiting for entropy. Only SecretsReady may start the listener.
      if (input == PortalInput::SecretsReady) {
        s->secrets_active = true;  // mark active ONLY after acknowledgement
        s->state = PortalState::SetupSession;
        a.start_listener = true;
      } else if (input == PortalInput::SecretsFailed) {
        beginClose(s, &a);  // fail closed: never expose a listener
      } else if (input == PortalInput::SessionCancel) {
        beginClose(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::SetupSession:
      // Liveness deadline (ticks) is handled above; an explicit cancel closes.
      if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      } else if (input == PortalInput::SessionCancel) {
        beginClose(s, &a);
      }
      // Candidate submission is ONLY via portalSessionSubmitCandidate.
      // ConfigureButton and STA events here are intentionally idempotent no-ops.
      break;

    case PortalState::CandidateQuiesce:
      // Waiting for the portal radio to be provably down before the trial.
      if (input == PortalInput::PortalQuiesced) {
        a.begin_candidate_trial = true;  // only now may a candidate connect
        nextStaConnectionId(s);          // fresh link generation for the trial
        a.ack_sta_connection_id = s->sta_connection_id;
        s->state = PortalState::CandidateTrial;
        startOp(s, &a);  // trial op; ack = CandidateConnected / CandidateFailed
      } else if (input == PortalInput::SessionCancel) {
        beginClose(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::CandidateTrial:
      if (input == PortalInput::CandidateConnected) {
        a.commit_candidate_flash = true;  // persist only after a real connect
        s->state = PortalState::Commit;
        startOp(s, &a);  // commit op; ack = CommitDone / CommitFailed
      } else if (input == PortalInput::CandidateFailed) {
        // Not expired (expiry is preempted above): restore the old config (if any)
        // under a fresh op and await RestoreDone before any AP-radio reopen.
        reopenSession(s, &a);
      } else if (input == PortalInput::SessionCancel) {
        beginClose(s, &a);  // ordered cancel + restore/idle
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::ReopenRestore:
      // A failed candidate's old-RAM-config restore is in flight. Await its
      // completion; NEVER let a PortalRadioReady stand in for it, and never start
      // a listener/AP radio until the restore is proven done.
      if (input == PortalInput::RestoreDone) {
        if (deadlineExpired(policy, now_ms, s->deadline_start_ms)) {
          // The deadline lapsed while restoring: do NOT reopen the portal. Settle
          // the restored old STA back to normal operation instead. Secrets are
          // preserved only for an unexpired reopen, so zeroize here; arm the
          // one-shot fetch for when the STA link returns.
          a.zeroize_secrets = true;
          s->secrets_active = false;
          s->pending_immediate_fetch = true;
          s->state = PortalState::StaConnecting;
          startStaConnect(s, &a);  // reconnect restored STA (mints STA id)
        } else {
          // Unexpired: reopen the portal. Secrets stay valid, so the next
          // PortalRadioReady goes straight to the listener. This is the ONLY path
          // that starts a new AP-radio preparation, and it emits no restore.
          s->state = PortalState::PortalRadioPrep;
          a.start_ap_radio = true;
          startOp(s, &a);  // NEW radio-prep op; ack = PortalRadioReady
        }
      } else if (input == PortalInput::RestoreFailed) {
        // Fail closed: nothing restored. Zeroize and go idle (bounded, not a
        // stall). Keep the pending fetch so a later real STA reconnect still
        // forces exactly one immediate fetch.
        a.zeroize_secrets = true;
        s->secrets_active = false;
        s->pending_immediate_fetch = true;
        s->state = PortalState::StaOfflineIdle;
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::Commit:
      if (input == PortalInput::CommitDone) {
        // The candidate is now the working config; secrets are no longer needed.
        // The candidate's link generation (minted at begin_candidate_trial and
        // still current) becomes the active normal-STA id, so a later StaLost must
        // echo it.
        a.zeroize_secrets = true;
        s->secrets_active = false;
        s->has_candidate = false;
        s->has_old_config = true;
        a.resume_adsb = true;
        a.force_immediate_adsb_fetch = true;
        s->pending_immediate_fetch = false;
        s->state = PortalState::StaOnline;
      } else if (input == PortalInput::CommitFailed) {
        // Bounded failure: the candidate may still own a LAN IP, so cancel it and
        // wait for the ack before restoring the untouched old credentials.
        beginClose(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::SetupCleanup:
      // The listener/AP stop is in flight; restore only after it is acknowledged.
      if (input == PortalInput::PortalQuiesced) {
        settleAfterCleanup(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::TrialCancel:
      // The candidate cancel is in flight; restore only after it is acknowledged.
      if (input == PortalInput::CandidateCancelled) {
        settleAfterCleanup(s, &a);
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::Restore:
      if (input == PortalInput::RestoreDone) {
        s->state = PortalState::StaConnecting;
        startStaConnect(s, &a);  // reconnect the restored STA path (mints STA id)
      } else if (input == PortalInput::RestoreFailed) {
        // Bounded failure: go idle instead of stalling. Keep the pending fetch so
        // a later real STA reconnect still forces exactly one immediate fetch.
        s->state = PortalState::StaOfflineIdle;
      } else if (input == PortalInput::EraseConfirmed) {
        enterFactoryErase(s, &a);
      }
      break;

    case PortalState::FactoryErase:
      if (input == PortalInput::EraseDone) {
        s->has_old_config = false;
        s->has_candidate = false;
        s->secrets_active = false;
        s->pending_immediate_fetch = false;
        s->state = PortalState::BootDecide;
      }
      break;
  }

  return a;
}

PortalActions portalSessionSubmitCandidate(PortalSession* s,
                                           const PortalSessionPolicy& policy,
                                           uint32_t now_ms,
                                           const AuthorizedProvisioning& sub) {
  PortalActions a = {};
  if (s == nullptr) {
    return a;
  }
  // Only a fully prepared SetupSession accepts a submission. Any other phase
  // means no session or an already-active trial -> reject (single-use per trial).
  if (s->state != PortalState::SetupSession) {
    return a;
  }
  if (deadlineExpired(policy, now_ms, s->deadline_start_ms)) {
    return a;  // expired: a tick will close the session; do not start a trial
  }
  // The authenticated capability must be authorized AND bound to THIS session.
  // This is what makes a mere validateProvisioning() length success insufficient,
  // and an authorized() flag cannot be forged outside authenticateProvisioning.
  if (!sub.authorized() || sub.session_id() != s->session_id) {
    return a;
  }

  s->trial_id += 1U;        // monotonic, nonzero for every accepted submission
  if (s->trial_id == 0U) {
    s->trial_id = 1U;
  }
  s->has_candidate = true;
  a.stop_listener = true;   // stop client + HTTP + captive DNS + SoftAP
  s->state = PortalState::CandidateQuiesce;
  startOp(s, &a);           // quiesce op; ack = PortalQuiesced
  return a;
}

bool portalListenerActive(PortalState state) {
  return state == PortalState::SetupSession;
}

bool portalAdsbActive(PortalState state) {
  return state == PortalState::StaOnline;
}

bool portalInSession(PortalState state) {
  switch (state) {
    case PortalState::PortalRadioPrep:
    case PortalState::PortalAwaitSecrets:
    case PortalState::SetupSession:
    case PortalState::CandidateQuiesce:
    case PortalState::CandidateTrial:
    case PortalState::ReopenRestore:
    case PortalState::Commit:
    case PortalState::SetupCleanup:
    case PortalState::TrialCancel:
    case PortalState::Restore:
      return true;
    default:
      return false;
  }
}

}  // namespace core
