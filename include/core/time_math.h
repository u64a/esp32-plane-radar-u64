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

// Remaining portion of a budget that began at started_ms, saturating at zero
// once the budget is exhausted. Rollover-safe: the unsigned (now_ms -
// started_ms) subtraction wraps correctly across the uint32 millis() rollover,
// so callers can chain cumulative budgets (e.g. DNS+TCP+TLS, or send+response)
// without a fresh full budget per phase. Exactly-expired (elapsed == budget)
// yields 0.
inline uint32_t remainingBudgetMs(uint32_t now_ms, uint32_t started_ms,
                                  uint32_t budget_ms) {
  const uint32_t elapsed = elapsedMs(now_ms, started_ms);
  return elapsed >= budget_ms ? 0U : budget_ms - elapsed;
}

}  // namespace core
