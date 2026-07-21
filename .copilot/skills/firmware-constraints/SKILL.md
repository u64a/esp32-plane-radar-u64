---
name: "firmware-constraints"
description: "ESP32-C3 resource and validation constraints for this firmware"
domain: "embedded"
confidence: "high"
source: "team-decision"
---

## Hardware Budget

- ESP32-C3, 160 MHz, 320 KB RAM, 4 MB flash, no PSRAM.
- Keep exactly one 240x240 RGB565 framebuffer: 115,200 bytes.
- Do not introduce a second full-screen pixel buffer.
- Bound every network-controlled buffer and collection explicitly.
- Keep the asynchronous network worker disabled by default until hardware measurements
  prove the framebuffer, verified TLS, queues, snapshots, and worker stack coexist.

## Engineering Rules

- Prefer fixed storage, pure logic modules, and rollover-safe unsigned time arithmetic.
- Publish aircraft snapshots only after complete successful parsing.
- Preserve existing NVS compatibility unless a migration is explicitly reviewed.
- Never use insecure TLS fallback or expose a permanent configuration listener.
- Do not log Wi-Fi or provisioning credentials.

## Validation Levels

- PlatformIO native tests prove deterministic logic.
- The headless LovyanGFX render gate ([env:native-gfx], test/test_native_gfx +
  test/golden BMPs; run by scripts/native-test.ps1) compiles the real production
  UI drawing code and byte-compares each rendered 240x240 RGB565 scene against a
  checked-in golden. It proves pixel geometry/text/layout and deterministic
  drawing; it is offline/headless (no SDL, window, or hardware) and does NOT
  prove panel colours, BGR order, inversion, SPI, or brightness.
- QEMU is optional and cannot prove Wi-Fi, TLS-over-Wi-Fi, or GC9A01 behavior;
  it is not a release gate.
- Real RF, captive portal, heap peaks, panel output, power, and soak behavior remain
  hardware-only acceptance gates.
