#pragma once

#include <cstdint>

// Provisioning button gesture state machine (pure, rollover-safe).
//
// A single physical button (sampled as `physical_down`) drives, from `now_ms`
// alone, three distinct intents plus a guarded factory-erase confirmation:
//
//   * Tap       -- a short press/release, e.g. cycle radar range.
//   * Configure -- a medium hold released in the configure window opens setup.
//   * Erase     -- a long (>= 8 s) hold ARMS erase, but the *same* hold can never
//                  erase. The user must release, then within a confirmation
//                  window start a *second* press and hold it long enough. This
//                  two-stage, release-required design means a stuck button (held
//                  continuously) can never trigger a destructive erase.
//
// All timing uses unsigned (now - start) arithmetic, so the machine is correct
// across the ~49-day millis() rollover. Events are one-shot (emitted on the tick
// the condition is first met); prompts are the current level-based UI hint.

namespace core {

// Timing policy (all milliseconds). Defaults encode the specified boundaries;
// the struct exists so tests can drive exact edges (and config can mirror it).
struct ProvisionButtonPolicy {
  uint32_t tap_min_ms;         // tap window is [tap_min, tap_max)
  uint32_t tap_max_ms;         // dead band is  [tap_max, configure_min)
  uint32_t configure_min_ms;   // configure window is [configure_min, arm)
  uint32_t arm_ms;             // hold >= arm arms erase
  uint32_t confirm_window_ms;  // deadline to START the second press after release
  uint32_t confirm_hold_ms;    // second press must be held >= this to confirm
};

inline constexpr ProvisionButtonPolicy kDefaultProvisionButtonPolicy = {
    /*tap_min_ms=*/40,
    /*tap_max_ms=*/1000,
    /*configure_min_ms=*/2000,
    /*arm_ms=*/8000,
    /*confirm_window_ms=*/10000,
    /*confirm_hold_ms=*/3000,
};

enum class ProvisionButtonState : uint8_t {
  Idle = 0,
  FirstPress,   // first press down; classifying tap/configure/arm
  EraseArmed,   // first press held >= arm and still down; release to continue
  ConfirmWait,  // released after arming; awaiting the second press
  ConfirmHold,  // second press down; timing toward confirm_hold
};

// One-shot events emitted on the tick the condition is first satisfied.
enum class ProvisionButtonEvent : uint8_t {
  None = 0,
  Tap,               // released in [tap_min, tap_max)
  ConfigureRequest,  // released in [configure_min, arm)
  EraseConfirmed,    // second press held >= confirm_hold within the window
  EraseCancelled,    // second press released early, or the window timed out
};

// Level-based UI prompt reflecting the current state.
enum class ProvisionButtonPrompt : uint8_t {
  None = 0,
  ReleaseToConfigure,  // first press in [configure_min, arm)
  EraseArmedRelease,   // first press held >= arm (armed; release to confirm)
  ConfirmHoldToErase,  // window open, awaiting the second press
  KeepHoldingToErase,  // second press down, not yet at confirm_hold
  Cancelled,           // shown alongside EraseCancelled
};

// Caller-owned state. Zero-initialize with provisionButtonInit (assumes the
// button starts released).
struct ProvisionButton {
  ProvisionButtonState state;
  bool prev_down;
  uint32_t press_started_ms;   // start of the current FirstPress/ConfirmHold press
  uint32_t window_started_ms;  // start of the ConfirmWait window
};

struct ProvisionButtonOutput {
  ProvisionButtonEvent event;
  ProvisionButtonPrompt prompt;
};

void provisionButtonInit(ProvisionButton* b);

// Advance the machine one tick. Call every loop with the current time and the
// debounced physical button level. Returns the one-shot event (if any) and the
// current prompt.
ProvisionButtonOutput provisionButtonUpdate(ProvisionButton* b,
                                            const ProvisionButtonPolicy& policy,
                                            uint32_t now_ms, bool physical_down);

}  // namespace core
