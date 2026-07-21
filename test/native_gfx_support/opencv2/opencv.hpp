#pragma once

// Headless native-gfx shim for <opencv2/opencv.hpp>.
//
// LovyanGFX 1.2.25 selects its platform backend from
// src/lgfx/v1/platforms/common.hpp. With no SDL2 present and no LGFX_LINUX_FB,
// the only self-contained desktop backend is the "opencv" one, whose selection
// is guarded solely by __has_include(<opencv2/opencv.hpp>). Choosing it links a
// panel-free platform layer (millis/heap via std::chrono/malloc) with NO SDL
// window -- exactly what a headless, offline sprite render harness needs.
//
// This fake header provides ONLY the tiny slice of the cv:: API that LovyanGFX's
// opencv/common.cpp and opencv/Panel_OpenCV.cpp reference, so both compile. The
// render harness uses an LGFX_Sprite canvas as `tft`; Panel_OpenCV is NEVER
// instantiated and none of these cv:: entry points is ever called at runtime, so
// the empty inline bodies are compile/link scaffolding only. Real OpenCV is not
// required or linked. Selected ONLY by the [env:native-gfx] include path.
//
// Note: pixel values in the harness never depend on wall-clock time, so
// TickMeter accuracy is irrelevant to golden determinism; it only needs to be
// monotonic so LovyanGFX's busy-wait delayMicroseconds() loop terminates.

#include <chrono>
#include <cstdint>
#include <string>

// OpenCV pixel-type macro used by Panel_OpenCV::init (value is irrelevant here).
#define CV_8UC3 16

namespace cv {

class TickMeter {
 public:
  TickMeter() { reset(); }

  void reset() {
    accumulated_us_ = 0.0;
    running_ = false;
  }

  void start() {
    start_ = std::chrono::steady_clock::now();
    running_ = true;
  }

  void stop() {
    if (!running_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    accumulated_us_ +=
        std::chrono::duration<double, std::micro>(now - start_).count();
    running_ = false;
  }

  double getTimeMilli() const { return accumulated_us_ / 1000.0; }
  double getTimeMicro() const { return accumulated_us_; }

 private:
  std::chrono::steady_clock::time_point start_{};
  double accumulated_us_ = 0.0;
  bool running_ = false;
};

// Minimal stand-in for cv::Mat. Only the members Panel_OpenCV touches exist; it
// owns no pixel storage because the panel is never initialized in the harness.
class Mat {
 public:
  Mat() = default;
  Mat(int /*rows*/, int /*cols*/, int /*type*/) {}
  void release() { data = nullptr; }
  uint8_t* data = nullptr;
};

// Window / event constants referenced by Panel_OpenCV (values unused).
enum {
  EVENT_LBUTTONDOWN = 1,
  EVENT_LBUTTONUP = 4,
  WND_PROP_AUTOSIZE = 1,
  WINDOW_AUTOSIZE = 1,
  COLOR_BGR2RGB = 4,
};

using MouseCallback = void (*)(int, int, int, int, void*);

// Compile/link-only HighGUI + imgproc stubs. Never called by the harness.
inline double getWindowProperty(const std::string& /*name*/, int /*prop*/) {
  return -1.0;
}
inline void namedWindow(const std::string& /*name*/, int /*flags*/) {}
inline void setMouseCallback(const std::string& /*name*/, MouseCallback /*cb*/,
                             void* /*userdata*/) {}
inline void cvtColor(const Mat& /*src*/, Mat& /*dst*/, int /*code*/) {}
inline void imshow(const std::string& /*name*/, const Mat& /*mat*/) {}
inline int waitKey(int /*delay_ms*/) { return -1; }

}  // namespace cv
