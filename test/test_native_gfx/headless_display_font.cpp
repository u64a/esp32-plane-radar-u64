// Headless native-gfx replacement for src/hardware/display_font.cpp.
//
// The firmware loads the anti-aliased VLW smooth font from an ESP32 embedded
// binary blob (_binary_data_ui_font_vlw_start/_end symbols emitted by
// board_build.embed_files). Those linker symbols do not exist on the native
// desktop toolchain. To exercise the SAME smooth-font drawing path as the panel,
// this shim loads the EXACT repository bytes of data/ui_font.vlw from disk into a
// persistent heap buffer and hands the same pointer to LovyanGFX's
// loadFont(..., ft_vlw), matching production behavior byte-for-byte.
//
// LovyanGFX's loadFont(const uint8_t*, ...) keeps a reference to the supplied
// bytes (it does not copy them), so the buffer is a function-local static that
// lives for the whole process. Selected ONLY by [env:native-gfx].

#include "hardware/display_font.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include "hardware/display.h"

#ifndef PLANE_RADAR_VLW_PATH
#define PLANE_RADAR_VLW_PATH "data/ui_font.vlw"
#endif

namespace {

bool s_vlw_loaded = false;

// Persistent VLW font bytes read from disk. LovyanGFX references this pointer for
// the lifetime of the loaded font, so the storage must outlive every draw.
std::vector<uint8_t>& vlwStorage() {
  static std::vector<uint8_t> storage = [] {
    std::vector<uint8_t> bytes;
    const char* path = PLANE_RADAR_VLW_PATH;
    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
      std::fprintf(stderr, "native-gfx: cannot open VLW font '%s'\n", path);
      return bytes;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (size > 0) {
      bytes.resize(static_cast<size_t>(size));
      const size_t read = std::fread(bytes.data(), 1, bytes.size(), fp);
      if (read != bytes.size()) {
        std::fprintf(stderr, "native-gfx: short read on VLW font '%s'\n", path);
        bytes.clear();
      }
    }
    std::fclose(fp);
    return bytes;
  }();
  return storage;
}

const uint8_t* vlwData() { return vlwStorage().data(); }

size_t vlwDataLen() { return vlwStorage().size(); }

bool vlwActiveOn(const lgfx::LGFXBase& gfx) {
  const lgfx::IFont* font = gfx.getFont();
  return font != nullptr && font->getType() == lgfx::IFont::font_type_t::ft_vlw;
}

}  // namespace

bool displayFontInit() {
  s_vlw_loaded = vlwDataLen() > 0 &&
                 tft.loadFont(vlwData(), lgfx::IFont::font_type_t::ft_vlw);
  if (!s_vlw_loaded) {
    std::fprintf(stderr,
                 "native-gfx: smooth font load failed -- bitmap fallback\n");
  }
  return s_vlw_loaded;
}

bool displayFontIsSmooth() { return s_vlw_loaded; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_vlw_loaded) {
    return false;
  }
  if (vlwActiveOn(gfx)) {
    return true;
  }
  return gfx.loadFont(vlwData(), lgfx::IFont::font_type_t::ft_vlw);
}

void displayFontSetSmoothSize(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
}

void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font) {
  gfx.setFont(font);
  gfx.setTextSize(1);
}
