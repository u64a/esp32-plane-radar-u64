#pragma once

#include <cstdint>

#include "core/portal_auth.h"  // AuthorizedProvisioning (candidate-submit gate)

// Portal / session + credential transaction model (pure, Arduino-free).
//
// This is the security-critical controller the ESP adapter drives. It decides
// when the setup listener (HTTP + captive DNS + SoftAP) may exist, how a
// candidate Wi-Fi credential is trialed without destroying the working one, and
// when a destructive factory erase is permitted. It owns policy only: every
// side effect is surfaced as a one-shot "action request" for the adapter to
// perform, so the following invariants are provable in native tests:
//
//   * The setup listener exists ONLY in SetupSession (portalListenerActive), and
//     it is reached ONLY through an explicit radio + secrets handshake: entering
//     setup first pauses ADS-B and disconnects STA and initializes the AP radio,
//     then waits for PortalRadioReady (STA has no IP AND the AP radio is up),
//     then requests secret generation and waits for SecretsReady before starting
//     the listener. An entropy failure (SecretsFailed) fails closed and never
//     exposes a listener. Secrets are NOT marked active until SecretsReady.
//   * A candidate connect (begin_candidate_trial) is emitted ONLY after the
//     adapter acknowledges PortalQuiesced -- i.e. the client, HTTP listener,
//     captive DNS, and SoftAP are all provably down. Submission and the trial are
//     separate transitions gated by that ack, so no ordering of the emitted
//     actions can ever connect a candidate while the listener is considered up.
//   * A successful candidate requests a FLASH commit and then closes; a failed
//     one re-enters the SAME session (same id/secrets, original deadline NOT
//     reset) -- but only if the deadline has not passed. When a stored config
//     exists the reopen also restores it to RAM; on FIRST boot (no stored
//     config) it reopens with no restore of nonexistent credentials. A failure
//     after expiry closes and never reopens. The working credentials in flash are
//     never overwritten until a candidate actually connects, so a power loss
//     mid-trial preserves them.
//   * The original session deadline is enforced across EVERY setup-lifetime state
//     and preempts EVERY non-destructive async success/failure: at or past the
//     deadline the machine never starts/reopens a listener, starts a candidate,
//     or commits one -- it drives ordered cleanup/rollback instead. timeout_ms ==
//     0 means disabled/unlimited, matching the firmware config convention. Only
//     EraseConfirmed may still take the physical destructive path past expiry.
//   * Close/rollback is ORDERED: a timeout/cancel while the listener or AP radio
//     is up first requests a listener stop and waits for PortalQuiesced before
//     any restore/reconnect (SetupCleanup); a timeout/cancel/commit-failure while
//     a candidate connect is in flight or connected first requests a candidate
//     cancel and waits for CandidateCancelled before any restore/reconnect
//     (TrialCancel). No transition ever emits restore/reconnect concurrently with
//     an unacknowledged listener stop or candidate cancellation, so a connected
//     candidate can never still own a LAN IP while the old config is restored.
//   * With a stored config, restore forces exactly one immediate ADS-B fetch,
//     only after the STA link is back. With NO stored config (first boot) it
//     zeroizes and goes OfflineIdle with no restore/reconnect and no pending
//     fetch until a real STA link appears.
//   * Commit and restore have bounded failure inputs (CommitFailed/RestoreFailed)
//     so a stuck flash/STA operation can never be a permanent terminal stall; a
//     commit failure keeps the old credentials and restores them only after the
//     candidate is provably cancelled.
//   * A candidate failure re-enters the SAME session, but old-config restore and
//     the AP-radio reopen are SEPARATE operation-bound steps. With a stored
//     config, CandidateFailed first disconnects and restores the old RAM config
//     (ReopenRestore, a FRESH operation) and awaits RestoreDone/RestoreFailed:
//     only RestoreDone starts a new AP-radio preparation, RestoreFailed fails
//     closed (zeroize, no listener, go idle), and no action set ever combines
//     restore-old with start-AP. First boot (no stored config) has nothing to
//     restore, so it starts radio prep directly after disconnect. If the deadline
//     expires while restoration is in flight, RestoreDone reconnects/settles the
//     old STA instead of reopening and never starts a listener; same
//     secrets/deadline are preserved only for an unexpired successful restore.
//   * Every stored/restored/candidate STA link attempt is generation-bound by a
//     monotonic nonzero sta_connection_id: each start_sta_connect and
//     begin_candidate_trial mints and exposes a fresh id, StaConnected/
//     StaConnectFailed echo it, the accepted connection retains it and StaLost
//     must match it, and on commit the candidate's id becomes the active
//     normal-STA id. A delayed link event from an earlier attempt is rejected. An
//     unsolicited StaConnected in OfflineIdle is NOT accepted: the adapter must
//     first request a new generation (StaRetry) before any link event applies.
//   * Delayed/stale async completions are rejected by IDENTITY. Every async side
//     effect stamps the actions with the identity the adapter must echo, and each
//     completion carries and must match that identity: the session_id (session-
//     scoped), the trial_id where applicable (trial-scoped), a monotonically
//     advancing nonzero operation_id minted per operation, AND the
//     sta_connection_id for STA-link and candidate-link events. Because the
//     operation_id advances even when the session/trial id does NOT (e.g. a
//     same-session reopen after a candidate failure), a delayed duplicate
//     completion from an earlier operation of the SAME session/trial is rejected
//     just like one from a prior session.
//   * factory_erase is requested only for a confirmed EraseConfirmed input; no
//     HTTP-originated input can ever reach it. Candidate submission is possible
//     only via portalSessionSubmitCandidate with an authenticated artifact.
//   * Close/cleanup is idempotent.
//
// Candidate/old credentials are modeled as opaque validity flags here; the actual
// secret bytes live in the adapter and are never logged.

namespace core {

enum class PortalState : uint8_t {
  BootDecide = 0,      // choosing the boot path from stored-credential presence
  StaConnecting,       // connecting STA with stored/old credentials (no listener)
  StaOnline,           // connected; normal operation (no listener; ADS-B active)
  StaOfflineIdle,      // saved Wi-Fi failed / first boot idle; NOT auto-opening
  PortalRadioPrep,     // entering setup: STA quiescing + AP radio initializing
  PortalAwaitSecrets,  // AP radio up; secret generation requested (no listener)
  SetupSession,        // the ONLY state with listener + captive DNS + SoftAP
  CandidateQuiesce,    // submit accepted; tearing the portal radio down (pre-trial)
  CandidateTrial,      // RAM-only trial of the candidate (portal provably down)
  ReopenRestore,       // candidate failed: restoring old RAM config (fresh op)
                       // BEFORE any AP-radio reopen; awaits RestoreDone/Failed
  Commit,              // persisting a successful candidate to flash
  SetupCleanup,        // close: listener/AP stop requested; awaiting PortalQuiesced
  TrialCancel,         // close: candidate cancel requested; awaiting CandidateCancelled
  Restore,             // restoring the old STA path on close (timeout/cancel)
  FactoryErase,        // performing a confirmed destructive erase
};

enum class PortalInput : uint8_t {
  None = 0,            // plain time tick (drives the session deadline)
  BootHasCredentials,  // boot: stored credentials exist
  BootNoCredentials,   // boot: no stored credentials
  StaConnected,        // STA link came up
  StaConnectFailed,    // STA connect attempt exhausted
  StaLost,             // STA link dropped from online
  StaRetry,            // adapter-initiated background reconnect from OfflineIdle:
                       // mints a fresh sta_connection_id and (re)starts STA connect
  ConfigureButton,     // physical configure gesture: open setup
  PortalRadioReady,    // ack: STA has no IP AND the AP radio is initialized
  SecretsReady,        // ack: per-session secrets generated
  SecretsFailed,       // entropy failure: secret generation failed (fail closed)
  PortalQuiesced,      // ack: client + HTTP + captive DNS + SoftAP are down
  CandidateConnected,  // candidate RAM trial connected
  CandidateFailed,     // candidate RAM trial failed
  CandidateCancelled,  // ack: an in-flight candidate connect was aborted/released
  SessionCancel,       // user cancelled the setup session
  EraseConfirmed,      // factory erase confirmed (from the button FSM)
  CommitDone,          // flash commit finished
  CommitFailed,        // flash commit failed (bounded; keep old credentials)
  RestoreDone,         // old-config restore finished
  RestoreFailed,       // old-config restore failed (bounded; go idle)
  EraseDone,           // factory erase finished
};

// One-shot requests the adapter must carry out for a transition. A zeroed struct
// means "do nothing".
//
// Radio/secrets sequencing is expressed through SEPARATE transitions rather than
// action ordering: start_ap_radio (mode switch to AP; no SSID/DNS/HTTP yet) is
// requested before secrets; start_listener (SoftAP SSID + captive DNS + HTTP)
// only after SecretsReady; begin_candidate_trial only after PortalQuiesced.
//
// When an action set initiates an async operation that expects a completion, the
// ack_* fields carry the identity the adapter MUST echo on that completion (see
// PortalAck). They are 0 when the transition starts no ack-expecting operation.
struct PortalActions {
  bool start_listener;               // start SoftAP(ssid,pw) + captive DNS + HTTP
  bool stop_listener;                // stop client + HTTP + captive DNS + SoftAP
  bool start_sta_connect;            // begin STA connect with stored/old creds
  bool disconnect_sta;               // disconnect STA / drop its IP (pre-AP)
  bool start_ap_radio;               // switch radio to AP mode / init AP netif
  bool pause_adsb;                   // pause ADS-B (retain snapshot AND backoff)
  bool resume_adsb;                  // resume the ADS-B path
  bool force_immediate_adsb_fetch;   // force exactly one immediate fetch now
  bool request_secret_generation;    // draw per-session secrets from entropy
  bool begin_candidate_trial;        // RAM-only candidate connect attempt
  bool cancel_candidate_trial;       // abort an in-flight candidate connect
  bool commit_candidate_flash;       // persist candidate credentials to flash
  bool restore_old_ram_config;       // restore previous RAM credentials
  bool zeroize_secrets;              // wipe portal password + CSRF token
  bool factory_erase;                // destructive erase (confirmed only)

  // Identity to echo on the completion for the async operation this transition
  // starts (0 when none). The adapter records these and returns them verbatim in
  // the PortalAck of the matching completion input.
  uint32_t ack_session_id;
  uint32_t ack_trial_id;
  uint32_t ack_operation_id;

  // STA link generation this transition begins (0 when it starts no stored/
  // restored/candidate STA attempt). Every start_sta_connect and
  // begin_candidate_trial exposes a fresh monotonic nonzero id here; the adapter
  // echoes it in PortalAck.sta_connection_id on StaConnected/StaConnectFailed/
  // StaLost (and, for a candidate attempt, on the candidate completion/cancel
  // events), so a delayed link event from an earlier attempt cannot be accepted
  // during a later one.
  uint32_t ack_sta_connection_id;
};

// Identity carried by an async completion input. The adapter echoes the ack_*
// fields it was handed when the operation was started. A completion whose triple
// does not match the current outstanding operation is a stale/delayed duplicate
// and is ignored. STA link events echo sta_connection_id instead (candidate link
// events echo BOTH the operation triple and sta_connection_id).
struct PortalAck {
  uint32_t session_id;
  uint32_t trial_id;
  uint32_t operation_id;
  uint32_t sta_connection_id;
};

struct PortalSession {
  PortalState state;
  uint32_t session_id;           // bumped when a NEW session opens (not on reopen)
  uint32_t trial_id;             // advanced (monotonic, nonzero) per candidate submit
  uint32_t operation_id;         // advanced (monotonic, nonzero) per async operation
  uint32_t sta_connection_id;    // advanced (monotonic, nonzero) per STA link attempt
  uint32_t deadline_start_ms;    // anchor for the session's liveness deadline
  bool has_old_config;           // a stored/previous STA config exists
  bool has_candidate;            // a candidate credential is loaded (RAM only)
  bool secrets_active;           // portal secrets live (ONLY after SecretsReady)
  bool pending_immediate_fetch;  // force one ADS-B fetch on the next StaConnected
};

struct PortalSessionPolicy {
  uint32_t session_timeout_ms;  // liveness deadline; 0 == disabled/unlimited
};

inline constexpr PortalSessionPolicy kDefaultPortalSessionPolicy = {
    /*session_timeout_ms=*/300000,  // 5 minutes
};

// Initialize to BootDecide with no credentials/candidate/secrets.
void portalSessionInit(PortalSession* s);

// Advance the machine. Returns the action requests for this transition.
//
// `ack` binds delayed async completions to the operation that produced them:
//   * session-scoped inputs (PortalRadioReady, SecretsReady, SecretsFailed,
//     PortalQuiesced, RestoreDone, RestoreFailed) must carry the current
//     session_id AND operation_id;
//   * trial-scoped inputs (CandidateConnected, CandidateFailed, CandidateCancelled)
//     must carry the current session_id, trial_id, operation_id AND
//     sta_connection_id (the candidate trial's link generation);
//   * flash-scoped inputs (CommitDone, CommitFailed) must carry the current
//     session_id, trial_id AND operation_id;
//   * STA link events (StaConnected, StaConnectFailed, StaLost) must carry the
//     current sta_connection_id (the generation of the outstanding/active STA
//     attempt); they are NOT bound to the portal operation_id.
// A completion whose identity does not match is ignored (returns a zeroed action
// set), so a delayed link event or async completion from an earlier attempt can
// never be accepted during a later one. For truly synchronous inputs
// (BootHasCredentials, ConfigureButton, StaRetry, EraseConfirmed, None) `ack` is
// unused; callers may leave it default-constructed.
PortalActions portalSessionUpdate(PortalSession* s,
                                  const PortalSessionPolicy& policy,
                                  uint32_t now_ms, PortalInput input,
                                  PortalAck ack = {});

// The ONLY way to submit a Wi-Fi candidate for trialing. It requires an
// AuthorizedProvisioning capability (minted solely by authenticateProvisioning:
// semantic validation + constant-time CSRF comparison + session binding); a mere
// validateProvisioning() length success cannot reach this path, and no caller can
// fabricate an authorized artifact. The submission is accepted only when:
//   * the machine is in a fully prepared SetupSession (listener active),
//   * the session deadline has not expired, and
//   * sub.authorized() is true and sub.session_id() == the current session_id.
// On acceptance it advances trial_id (monotonic, nonzero) and operation_id, loads
// the candidate, and emits stop_listener, moving to CandidateQuiesce. The RAM-only
// trial begins only later, when the adapter acknowledges PortalQuiesced. A second
// submission while a trial is already active (any non-SetupSession phase) is
// rejected; after a candidate failure the SAME session token may authorize a
// corrected new submission with a new trial_id while the ORIGINAL deadline keeps
// running.
PortalActions portalSessionSubmitCandidate(PortalSession* s,
                                           const PortalSessionPolicy& policy,
                                           uint32_t now_ms,
                                           const AuthorizedProvisioning& sub);

// Pure predicates (also the ADS-B pause/resume model):
//   * the listener is active ONLY in SetupSession (never in preparation,
//     quiesce, trial, commit, cleanup, or restore);
//   * ADS-B fetching is active ONLY in StaOnline (paused everywhere else);
//   * a setup session (any preparation/listener/candidate/commit/cleanup/restore
//     phase) is "in session", during which ADS-B stays paused.
bool portalListenerActive(PortalState state);
bool portalAdsbActive(PortalState state);
bool portalInSession(PortalState state);

}  // namespace core
