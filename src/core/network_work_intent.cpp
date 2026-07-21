#include "core/network_work_intent.h"

namespace core {

void networkWorkIntentInit(NetworkWorkIntentState* state) {
  if (state == nullptr) {
    return;
  }
  state->pending = NetworkWorkIntent::None;
}

void requestConfigure(NetworkWorkIntentState* state) {
  if (state == nullptr) {
    return;
  }
  // Configure never overrides a pending Erase (a factory wipe outranks a mere
  // reconfigure). Otherwise it is idempotent.
  if (state->pending == NetworkWorkIntent::Erase) {
    return;
  }
  state->pending = NetworkWorkIntent::Configure;
}

void requestErase(NetworkWorkIntentState* state) {
  if (state == nullptr) {
    return;
  }
  // Erase supersedes any pending Configure and is idempotent.
  state->pending = NetworkWorkIntent::Erase;
}

NetworkWorkIntent pendingIntent(const NetworkWorkIntentState& state) {
  return state.pending;
}

NetworkWorkIntent consumeIntent(NetworkWorkIntentState* state,
                                bool worker_quiesced) {
  if (state == nullptr || !worker_quiesced ||
      state->pending == NetworkWorkIntent::None) {
    // Not quiescent yet (or nothing pending): the intent remains latched so it
    // fires on a later tick once the worker is proven quiescent.
    return NetworkWorkIntent::None;
  }
  const NetworkWorkIntent intent = state->pending;
  state->pending = NetworkWorkIntent::None;  // one-shot
  return intent;
}

}  // namespace core
