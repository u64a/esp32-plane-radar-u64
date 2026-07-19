#pragma once

#include <cstdint>

namespace test_support {

class ManualClock {
 public:
  explicit ManualClock(uint32_t now_ms = 0) : now_ms_(now_ms) {}

  uint32_t nowMs() const { return now_ms_; }
  void set(uint32_t now_ms) { now_ms_ = now_ms; }
  void advance(uint32_t elapsed_ms) { now_ms_ += elapsed_ms; }

 private:
  uint32_t now_ms_;
};

}  // namespace test_support
