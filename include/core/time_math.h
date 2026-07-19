#pragma once

#include <cstdint>

namespace core {

inline uint32_t elapsedMs(uint32_t now_ms, uint32_t started_ms) {
  return now_ms - started_ms;
}

inline bool elapsedAtLeast(uint32_t now_ms, uint32_t started_ms,
                           uint32_t duration_ms) {
  return elapsedMs(now_ms, started_ms) >= duration_ms;
}

}  // namespace core
