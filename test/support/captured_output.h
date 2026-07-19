#pragma once

#include <string>
#include <string_view>

namespace test_support {

class CapturedOutput {
 public:
  void write(std::string_view text) { text_.append(text); }

  void writeln(std::string_view line) {
    write(line);
    text_.push_back('\n');
  }

  const std::string& text() const { return text_; }
  void clear() { text_.clear(); }

 private:
  std::string text_;
};

}  // namespace test_support
