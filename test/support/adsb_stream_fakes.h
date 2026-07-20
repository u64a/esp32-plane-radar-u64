#pragma once

// Scripted transport doubles for the Phase 5 bounded HTTP decoder and streaming
// parser. They stay independent of Arduino and production configuration: only
// the Arduino-free transport seams are implemented. A ScriptedByteSource can
// replay any response at any fragmentation (including one byte per read and
// every split offset), inject WouldBlock, and terminate with a clean end, a
// transport error, or an indefinite stall.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "services/adsb_transport.h"

namespace test_support {

class ScriptedByteSource : public services::adsb::ByteSource {
 public:
  enum class Terminal { End, Error, Stall };

  ScriptedByteSource(std::string data, size_t fragment, Terminal terminal,
                     bool inject_wouldblock = false)
      : data_(std::move(data)),
        fragment_(fragment == 0 ? 1 : fragment),
        terminal_(terminal),
        inject_wouldblock_(inject_wouldblock) {}

  services::adsb::ReadStatus read(uint8_t* buffer, size_t capacity,
                                  size_t* out_len) override {
    using services::adsb::ReadStatus;
    if (inject_wouldblock_ && !blocked_once_) {
      blocked_once_ = true;
      return ReadStatus::WouldBlock;
    }
    blocked_once_ = false;
    if (pos_ >= data_.size()) {
      if (terminal_ == Terminal::End) return ReadStatus::End;
      if (terminal_ == Terminal::Error) return ReadStatus::Error;
      return ReadStatus::WouldBlock;  // stall indefinitely
    }
    size_t n = data_.size() - pos_;
    if (n > fragment_) n = fragment_;
    if (n > capacity) n = capacity;
    std::memcpy(buffer, data_.data() + pos_, n);
    pos_ += n;
    *out_len = n;
    return ReadStatus::Data;
  }

 private:
  std::string data_;
  size_t fragment_;
  Terminal terminal_;
  bool inject_wouldblock_;
  size_t pos_ = 0;
  bool blocked_once_ = false;
};

class CountingClock : public services::adsb::Clock {
 public:
  explicit CountingClock(uint32_t start = 0) : now_(start) {}
  uint32_t nowMs() const override { return now_; }
  void advance(uint32_t ms) { now_ += ms; }
  uint32_t now_;
};

class AdvancingIdle : public services::adsb::IdleHandler {
 public:
  AdvancingIdle(CountingClock* clock, uint32_t step_ms)
      : clock_(clock), step_ms_(step_ms) {}
  void onIdle() override {
    ++calls;
    clock_->advance(step_ms_);
  }
  CountingClock* clock_;
  uint32_t step_ms_;
  uint32_t calls = 0;
};

}  // namespace test_support
