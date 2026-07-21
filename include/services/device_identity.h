#pragma once

// Device identity + entropy bridge for the secure setup portal (ESP-only).
//
// Supplies the two facts the pure portal-secrets core needs from hardware and
// nothing else: the factory MAC used to derive the stable public SoftAP SSID, and
// an injected cryptographic entropy source backed by esp_fill_random. This module
// NEVER logs a secret and never derives entropy from the MAC, time, or a counter.

#include <cstddef>
#include <cstdint>

#include "core/portal_secrets.h"

namespace services::identity {

// Read the factory station MAC (the stable per-device base MAC) into out[6]. This
// is the MAC the core derives the public "PlaneRadar-XXYYZZ" SSID from, so the
// visible SSID is stable across reboots and independent of radio mode. Returns
// false if the read fails.
bool factoryMac(uint8_t out[6]);

// Read the SoftAP interface MAC into out[6] (the BSSID the AP advertises).
// Returns false if the read fails.
bool softApMac(uint8_t out[6]);

// The injected cryptographic entropy source (esp_fill_random). It is strong only
// while the RF subsystem is active, which is why the portal keeps the STA radio
// up before drawing secrets. fill() never logs and returns false only on a
// null/oversized request.
core::EntropySource entropySource();

}  // namespace services::identity
