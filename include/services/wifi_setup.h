#pragma once

#include <cstdint>

// Secure Wi-Fi provisioning + runtime link controller (ESP-only facade).
//
// This module owns the whole Wi-Fi lifecycle around the pure core::PortalSession:
// boot connect, the temporary secure setup portal, the non-forgeable credential
// transaction, background reconnects, the two-stage provisioning button, factory
// erase, and the status-screen ownership during all of the above. main.cpp drives
// it through this small facade and stays out of the reconnect/portal business.

// One-time early init: GPIO + button ISR + state machines. Call before any
// networking. Safe to call more than once.
void wifiControllerInit();

// Register the filtered Wi-Fi event callbacks (STA GOT_IP + DISCONNECTED) BEFORE
// any WiFi.begin so a mid-fetch disconnect that auto-reconnects is still observed.
// Idempotent.
void wifiRegisterEventHandlers();

// Boot flow: capture the working credentials, then either connect the saved
// network (with the connecting UI) or, on first boot with no stored credentials,
// open the secure setup portal automatically. Blocks only during the boot connect
// UI; returns once the machine has settled (online, offline-idle, or setup open).
void wifiBootConnect();

// Per-loop pump. Advances the portal/session FSM, executes its actions, services
// background reconnects, pumps the captive portal + button, and owns the status
// screen whenever wifiOwnsDisplay() is true. Also serves as the ADS-B poll hook
// (services::adsb::setPollFn) so button/DNS stay responsive during blocking reads.
void wifiLoop();

// True when a STA IP is currently valid (level-based link state).
bool wifiLinkUp();

// True while the controller owns the panel: any setup-session phase, the boot
// connecting screen, or an in-progress button-gesture prompt. main skips ADS-B and
// radar redraw while this is true so status screens are not overdrawn.
bool wifiOwnsDisplay();

// One-shot latch: the controller asks main to force exactly one immediate ADS-B
// fetch (restored / newly committed STA link came up). Returns true once, then clears.
bool wifiConsumeImmediateFetch();

// One-shot latch: a physical tap requested the range action. Returns true once.
bool wifiConsumeRangeTap();

// Monotonic 32-bit count of STA disconnect events since boot (Phase 6 mid-fetch
// flap detection). Written only from the Arduino event task, read lock-free from
// the main loop. Survives auto-reconnect; wraps at 2^32.
uint32_t wifiDisconnectSeq();

// Main-owned hooks that let the Wi-Fi controller defer radio/NVS mutations
// (Configure re-open, factory Erase) until the OPTIONAL ADS-B network worker is
// provably quiesced, WITHOUT this header depending on FreeRTOS or the worker.
// Fixed function pointers + one context keep the coupling Arduino-free. All three
// callbacks run on the main task and MUST be non-blocking:
//   * request_pause -- idempotently ask the worker to pause (cooperative).
//   * quiesced      -- true ONLY when the worker is provably Paused AND every
//                      taken result/candidate has been resolved by main.
//   * resume        -- return a paused worker to service.
// The default (no hooks registered) keeps the immediate, worker-free Configure/
// Erase behavior: quiescence is treated as trivially true.
struct WifiNetworkWorkHooks {
  void (*request_pause)(void* ctx);
  bool (*quiesced)(void* ctx);
  void (*resume)(void* ctx);
  void* ctx;
};

// Register the network-work hooks (copied by value). Only the worker-enabled
// build calls this; in the default build it is an inert no-op.
void wifiSetNetworkWorkHooks(const WifiNetworkWorkHooks& hooks);
