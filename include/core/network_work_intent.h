#pragma once

// Pure, Arduino-free latch for a pending "heavy" network-management intent that
// must not run while the optional ADS-B worker might still be touching the radio
// or NVS. Configure (re-open secure provisioning) and Erase (factory wipe) both
// tear down or reconfigure the network stack, so they are deferred until the
// worker is PROVABLY quiesced (see core::workerQuiesced). This module only tracks
// which intent is pending and releases it exactly once the caller reports
// quiescence; the actual configure/erase side effects live in the integration
// layer.

#include <cstdint>
#include <type_traits>

namespace core {

enum class NetworkWorkIntent : uint8_t {
  None,       // nothing pending
  Configure,  // re-open secure provisioning once the worker is quiesced
  Erase,      // factory erase once the worker is quiesced (supersedes Configure)
};

// Fixed POD latch. Trivially copyable so it can be snapshotted across a lock.
struct NetworkWorkIntentState {
  NetworkWorkIntent pending;
};

// Seed an empty latch (nothing pending).
void networkWorkIntentInit(NetworkWorkIntentState* state);

// Request a Configure. Idempotent (a second Configure changes nothing) and it
// NEVER overrides a pending Erase -- an erase in progress must not be downgraded
// to a mere reconfigure.
void requestConfigure(NetworkWorkIntentState* state);

// Request an Erase. Erase supersedes any pending Configure (a factory wipe is
// strictly stronger) and is idempotent.
void requestErase(NetworkWorkIntentState* state);

// The intent currently latched, without consuming it.
NetworkWorkIntent pendingIntent(const NetworkWorkIntentState& state);

// Try to consume the pending intent. Returns None and leaves the latch untouched
// when nothing is pending OR when worker_quiesced is false -- so no configure or
// erase action is ever released before the worker is proven quiescent. When an
// intent is pending AND worker_quiesced is true it returns that intent and clears
// the latch to None (one-shot: a second call returns None until a new request).
NetworkWorkIntent consumeIntent(NetworkWorkIntentState* state,
                                bool worker_quiesced);

static_assert(std::is_trivially_copyable<NetworkWorkIntentState>::value,
              "NetworkWorkIntentState must be trivially copyable");
static_assert(sizeof(NetworkWorkIntent) == 1,
              "NetworkWorkIntent is a fixed 1-byte enum");

}  // namespace core
