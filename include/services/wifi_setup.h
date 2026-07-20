#pragma once

#include <cstdint>

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();

/**
 * Register the filtered Arduino-ESP32 Wi-Fi event callback(s). Currently hooks
 * ARDUINO_EVENT_WIFI_STA_DISCONNECTED to advance a monotonic disconnect counter.
 * Idempotent; call exactly once, before any network setup (WiFi.begin), so a
 * disconnect that occurs entirely inside an otherwise-blocking DNS/TCP/TLS fetch
 * is still observed even if the link auto-reconnects before the fetch returns.
 */
void wifiRegisterEventHandlers();

/**
 * Monotonic 32-bit count of STA disconnect events since boot. Written only from
 * the Arduino event task and read from the main loop via a lock-free atomic, so
 * it is race-free. It survives auto-reconnect: comparing a value captured before
 * a fetch with one captured after reveals a mid-fetch "flap" that the level-based
 * WiFi.status() cannot (it reads connected both before and after). Wraps at 2^32.
 */
uint32_t wifiDisconnectSeq();
