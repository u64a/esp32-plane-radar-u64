#pragma once

// Deterministic capture + golden-image gate for the native-gfx render harness.
//
// capture() reads the final 240x240 RGB framebuffer out of the headless canvas
// (the exact pixels produced after sprite push / direct drawing) and encodes it
// as an uncompressed 24-bit BMP -- a format any standard image tool can open, so
// the coordinator can visually inspect goldens and mismatches. The initial gate
// is exact byte comparison against the checked-in golden.
//
// Normal runs are READ-ONLY: they never write goldens. A mismatch writes the
// actual image to an ignored .pio path and fails. Goldens are (re)written ONLY
// when the environment variable PLANE_RADAR_UPDATE_GOLDENS is set to an
// unmistakable opt-in value ("1"), and every written file is printed.
//
// Colour note: the BMP stores the raw RGB565 framebuffer decoded to RGB888. The
// GC9A01 panel's BGR channel order and colour inversion are applied by hardware
// and are NOT modelled here, so on-screen colours are NOT proven -- only pixel
// geometry, text, and deterministic byte-for-byte stability are.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hardware/display.h"
#include "ui/radar_theme.h"

namespace nativegfx {

constexpr int kW = ui::radar::kSize;
constexpr int kH = ui::radar::kSize;

// Read the whole canvas as RGB888 (row-major, top-down). LovyanGFX converts from
// the internal 16bpp storage to rgb888 for us, independent of pixel byte order.
inline std::vector<lgfx::rgb888_t> captureRgb() {
  std::vector<lgfx::rgb888_t> pixels(static_cast<size_t>(kW) * kH);
  tft.readRect(0, 0, kW, kH, pixels.data());
  return pixels;
}

inline void putLE16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

inline void putLE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

// Encode the captured framebuffer as an uncompressed bottom-up 24-bit BMP.
inline std::vector<uint8_t> encodeBmp(const std::vector<lgfx::rgb888_t>& px) {
  const uint32_t row_stride =
      static_cast<uint32_t>(((24 * kW + 31) / 32) * 4);
  const uint32_t image_size = row_stride * static_cast<uint32_t>(kH);
  const uint32_t data_offset = 54;
  const uint32_t file_size = data_offset + image_size;

  std::vector<uint8_t> bmp;
  bmp.reserve(file_size);
  bmp.push_back('B');
  bmp.push_back('M');
  putLE32(bmp, file_size);
  putLE32(bmp, 0);
  putLE32(bmp, data_offset);

  putLE32(bmp, 40);  // BITMAPINFOHEADER size
  putLE32(bmp, static_cast<uint32_t>(kW));
  putLE32(bmp, static_cast<uint32_t>(kH));  // positive -> bottom-up
  putLE16(bmp, 1);                          // planes
  putLE16(bmp, 24);                         // bits per pixel
  putLE32(bmp, 0);                          // BI_RGB, no compression
  putLE32(bmp, image_size);
  putLE32(bmp, 2835);  // 72 DPI x
  putLE32(bmp, 2835);  // 72 DPI y
  putLE32(bmp, 0);
  putLE32(bmp, 0);

  const uint32_t pad = row_stride - static_cast<uint32_t>(kW) * 3;
  for (int y = kH - 1; y >= 0; --y) {  // BMP rows are bottom-up
    const lgfx::rgb888_t* row = &px[static_cast<size_t>(y) * kW];
    for (int x = 0; x < kW; ++x) {
      bmp.push_back(row[x].B8());
      bmp.push_back(row[x].G8());
      bmp.push_back(row[x].R8());
    }
    for (uint32_t p = 0; p < pad; ++p) {
      bmp.push_back(0);
    }
  }
  return bmp;
}

inline std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::vector<uint8_t> bytes;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return bytes;
  }
  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), fp) != bytes.size()) {
      bytes.clear();
    }
  }
  std::fclose(fp);
  return bytes;
}

inline bool writeFileBytes(const std::string& path,
                           const std::vector<uint8_t>& bytes) {
  const std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) {
    return false;
  }
  const size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), fp);
  std::fclose(fp);
  return wrote == bytes.size();
}

inline std::string goldenPath(const char* scene) {
  return std::string("test/golden/") + scene + ".bmp";
}

inline std::string actualPath(const char* scene) {
  return std::string(".pio/native-gfx-out/") + scene + ".actual.bmp";
}

inline bool updateModeEnabled() {
  const char* v = std::getenv("PLANE_RADAR_UPDATE_GOLDENS");
  return v != nullptr && std::strcmp(v, "1") == 0;
}

// Capture the current canvas and gate it against scene's golden BMP. Returns true
// when the render matches the golden (or when a golden was written in the
// opt-in update mode). On mismatch, writes the actual image under .pio for
// inspection and returns false.
inline bool checkGolden(const char* scene) {
  const std::vector<uint8_t> actual = encodeBmp(captureRgb());
  const std::string golden = goldenPath(scene);

  if (updateModeEnabled()) {
    if (!writeFileBytes(golden, actual)) {
      std::fprintf(stderr, "native-gfx: FAILED to write golden %s\n",
                   golden.c_str());
      return false;
    }
    std::printf("native-gfx: UPDATED golden %s (%zu bytes)\n", golden.c_str(),
                actual.size());
    return true;
  }

  const std::vector<uint8_t> expected = readFileBytes(golden);
  if (expected.empty()) {
    const std::string out = actualPath(scene);
    writeFileBytes(out, actual);
    std::fprintf(stderr,
                 "native-gfx: MISSING golden %s -- wrote actual to %s. Run with "
                 "PLANE_RADAR_UPDATE_GOLDENS=1 to create goldens.\n",
                 golden.c_str(), out.c_str());
    return false;
  }
  if (expected.size() == actual.size() &&
      std::memcmp(expected.data(), actual.data(), actual.size()) == 0) {
    return true;
  }
  const std::string out = actualPath(scene);
  writeFileBytes(out, actual);
  std::fprintf(stderr,
               "native-gfx: MISMATCH scene '%s' (golden %zu B, actual %zu B). "
               "Actual written to %s.\n",
               scene, expected.size(), actual.size(), out.c_str());
  return false;
}

}  // namespace nativegfx
