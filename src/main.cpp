/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "core/poll_policy.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
core::ReconnectState g_reconnect_state = {};
core::AdsbPollState g_adsb_poll_state = {};

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    core::adsbRadarHidden(&g_adsb_poll_state);
    return;
  }
  ui::radarDisplayDraw();
  g_radar_visible = true;
  core::adsbRadarDisplayed(&g_adsb_poll_state);
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  const bool frame_buffered = ui::radarDisplayPrepareFrame();
  Serial.printf("radar: rendering mode: %s\n",
                frame_buffered ? "frame sprite" : "direct draw");
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleBootButton();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
      core::adsbRadarHidden(&g_adsb_poll_state);
    }

    const uint32_t now_ms = millis();
    core::reconnectDisconnected(&g_reconnect_state, now_ms);
    if (core::reconnectAttemptDue(
            g_reconnect_state, now_ms, config::kWifiDownGraceMs,
            config::kWifiReconnectIntervalMs)) {
      const bool connected = wifiReconnect();
      core::reconnectAttemptCompleted(&g_reconnect_state, millis(), connected);
      if (connected) {
        showRadarIfConnected();
      }
    }
  } else {
    core::reconnectConnected(&g_reconnect_state);
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (core::adsbFetchDue(g_adsb_poll_state, millis(),
                                  config::kAdsbFetchIntervalMs)) {
      fetchAndDrawAircraft();
      core::adsbFetchCompleted(&g_adsb_poll_state, millis());
    }
  }

  delay(10);
}
