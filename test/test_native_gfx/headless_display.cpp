// Headless native-gfx replacement for src/hardware/display.cpp.
//
// Provides the global `tft` canvas and displayInit() for the offline render
// harness. Unlike the firmware displayInit() (which drives the GC9A01 over SPI),
// this allocates a 240x240 RGB565 in-RAM sprite canvas so the production UI
// drawing code renders into a deterministic framebuffer we can capture. Selected
// ONLY by [env:native-gfx] (the firmware display.cpp is filtered out there).

#include "hardware/display.h"

#include <cstdio>

#include "hardware/display_font.h"
#include "ui/radar_theme.h"

LGFX tft;

void displayInit() {
  tft.setColorDepth(16);
  if (tft.createSprite(ui::radar::kSize, ui::radar::kSize) == nullptr) {
    std::fprintf(stderr, "native-gfx: failed to allocate %dx%d canvas\n",
                 ui::radar::kSize, ui::radar::kSize);
    return;
  }
  tft.setRotation(0);
  tft.setTextWrap(false);
  displayFontInit();
}
