#pragma once

// Headless native-gfx shim for include/hardware/lgfx_config.hpp.
//
// The firmware header defines LGFX as a GC9A01-on-SPI lgfx::LGFX_Device. That
// device pulls in ESP32 bus/panel platform code that does not exist on the
// native desktop toolchain and would drive real SPI. For the offline render
// harness we instead define LGFX as a standalone LovyanGFX sprite canvas: an
// in-RAM 240x240 RGB565 framebuffer that runs the SAME LGFXBase drawing engine,
// fonts, and sprite compositing as the panel, but with zero hardware, window, or
// SDL dependency. displayInit() (test/test_native_gfx/headless_display.cpp)
// allocates the 240x240 canvas at 16bpp.
//
// Selected ONLY by the [env:native-gfx] -iquote include path (see
// platformio.ini). Firmware builds keep the real GC9A01 device header, so this
// shim can never change the production display driver.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

// Headless off-screen canvas standing in for the GC9A01 panel. It IS-A
// LovyanGFX (via LGFX_Sprite), so the production `LGFX_Sprite s_frame(&tft)`
// double-buffer in ui/radar_display.cpp composites into and pushes onto it
// exactly as it would onto the real device.
class LGFX : public lgfx::LGFX_Sprite {
 public:
  LGFX() : lgfx::LGFX_Sprite() {}
};
