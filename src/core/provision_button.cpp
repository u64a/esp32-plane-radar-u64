#include "core/provision_button.h"

#include "core/time_math.h"

namespace core {

namespace {

// Current level prompt for the (already-updated) state.
ProvisionButtonPrompt computePrompt(const ProvisionButton& b,
                                    const ProvisionButtonPolicy& p,
                                    uint32_t now_ms) {
  switch (b.state) {
    case ProvisionButtonState::FirstPress: {
      const uint32_t held = elapsedMs(now_ms, b.press_started_ms);
      if (held >= p.arm_ms) {
        return ProvisionButtonPrompt::EraseArmedRelease;  // safety net
      }
      if (held >= p.configure_min_ms) {
        return ProvisionButtonPrompt::ReleaseToConfigure;
      }
      return ProvisionButtonPrompt::None;
    }
    case ProvisionButtonState::EraseArmed:
      return ProvisionButtonPrompt::EraseArmedRelease;
    case ProvisionButtonState::ConfirmWait:
      return ProvisionButtonPrompt::ConfirmHoldToErase;
    case ProvisionButtonState::ConfirmHold:
      return ProvisionButtonPrompt::KeepHoldingToErase;
    case ProvisionButtonState::Idle:
    default:
      return ProvisionButtonPrompt::None;
  }
}

}  // namespace

void provisionButtonInit(ProvisionButton* b) {
  if (b == nullptr) {
    return;
  }
  b->state = ProvisionButtonState::Idle;
  b->prev_down = false;
  b->press_started_ms = 0;
  b->window_started_ms = 0;
}

ProvisionButtonOutput provisionButtonUpdate(ProvisionButton* b,
                                            const ProvisionButtonPolicy& policy,
                                            uint32_t now_ms, bool physical_down) {
  ProvisionButtonOutput out = {ProvisionButtonEvent::None,
                               ProvisionButtonPrompt::None};
  if (b == nullptr) {
    return out;
  }
  const bool press_edge = physical_down && !b->prev_down;
  const bool release_edge = !physical_down && b->prev_down;

  switch (b->state) {
    case ProvisionButtonState::Idle:
      if (press_edge) {
        b->state = ProvisionButtonState::FirstPress;
        b->press_started_ms = now_ms;
      }
      break;

    case ProvisionButtonState::FirstPress: {
      const uint32_t held = elapsedMs(now_ms, b->press_started_ms);
      if (release_edge) {
        if (held >= policy.arm_ms) {
          // Reached the arm threshold (coarse tick skipped the down sample):
          // treat the release as arm+release and open the confirmation window.
          b->state = ProvisionButtonState::ConfirmWait;
          b->window_started_ms = now_ms;
        } else if (held >= policy.configure_min_ms) {
          out.event = ProvisionButtonEvent::ConfigureRequest;
          b->state = ProvisionButtonState::Idle;
        } else if (held >= policy.tap_max_ms) {
          b->state = ProvisionButtonState::Idle;  // dead band: no action
        } else if (held >= policy.tap_min_ms) {
          out.event = ProvisionButtonEvent::Tap;
          b->state = ProvisionButtonState::Idle;
        } else {
          b->state = ProvisionButtonState::Idle;  // sub-debounce: no action
        }
      } else if (physical_down && held >= policy.arm_ms) {
        b->state = ProvisionButtonState::EraseArmed;  // arm while still held
      }
      break;
    }

    case ProvisionButtonState::EraseArmed:
      // The same continuous hold can never erase; a release is required to move
      // toward confirmation. A stuck (never-released) button stays armed forever.
      if (release_edge) {
        b->state = ProvisionButtonState::ConfirmWait;
        b->window_started_ms = now_ms;
      }
      break;

    case ProvisionButtonState::ConfirmWait: {
      const uint32_t win = elapsedMs(now_ms, b->window_started_ms);
      if (win >= policy.confirm_window_ms) {
        out.event = ProvisionButtonEvent::EraseCancelled;
        out.prompt = ProvisionButtonPrompt::Cancelled;
        b->state = ProvisionButtonState::Idle;  // no second press: timed out
      } else if (press_edge) {
        b->state = ProvisionButtonState::ConfirmHold;
        b->press_started_ms = now_ms;
      }
      break;
    }

    case ProvisionButtonState::ConfirmHold: {
      const uint32_t held2 = elapsedMs(now_ms, b->press_started_ms);
      if (release_edge) {
        out.event = ProvisionButtonEvent::EraseCancelled;
        out.prompt = ProvisionButtonPrompt::Cancelled;
        b->state = ProvisionButtonState::Idle;  // released early: cancelled
      } else if (held2 >= policy.confirm_hold_ms) {
        out.event = ProvisionButtonEvent::EraseConfirmed;
        b->state = ProvisionButtonState::Idle;
      }
      break;
    }
  }

  if (out.prompt == ProvisionButtonPrompt::None) {
    out.prompt = computePrompt(*b, policy, now_ms);
  }
  b->prev_down = physical_down;
  return out;
}

}  // namespace core
