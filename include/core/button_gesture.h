#pragma once

#include <cstdint>

#include "core/time_math.h"

#if defined(__GNUC__)
#define CORE_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define CORE_ALWAYS_INLINE static inline
#endif

namespace core {

enum class ButtonEvent : uint8_t {
  Ignored,
  Tap,
  LongHold,
};

CORE_ALWAYS_INLINE ButtonEvent classifyReleasedPress(uint32_t held_ms,
                                                     uint32_t tap_min_ms,
                                                     uint32_t hold_ms) {
  if (held_ms < tap_min_ms) {
    return ButtonEvent::Ignored;
  }
  if (held_ms < hold_ms) {
    return ButtonEvent::Tap;
  }
  return ButtonEvent::LongHold;
}

CORE_ALWAYS_INLINE ButtonEvent classifyActiveHold(uint32_t now_ms,
                                                  uint32_t pressed_ms,
                                                  uint32_t hold_ms,
                                                  bool already_emitted) {
  return !already_emitted && elapsedAtLeast(now_ms, pressed_ms, hold_ms)
             ? ButtonEvent::LongHold
             : ButtonEvent::Ignored;
}

}  // namespace core

#undef CORE_ALWAYS_INLINE
