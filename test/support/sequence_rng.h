#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace test_support {

class SequenceRng {
 public:
  SequenceRng(std::initializer_list<uint32_t> values) : values_(values) {}

  uint32_t next() {
    if (cursor_ >= values_.size()) {
      throw std::out_of_range("SequenceRng exhausted");
    }
    return values_[cursor_++];
  }

  size_t remaining() const { return values_.size() - cursor_; }

 private:
  std::vector<uint32_t> values_;
  size_t cursor_ = 0;
};

}  // namespace test_support
