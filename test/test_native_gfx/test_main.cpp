// Native-gfx render golden suite (Unity).
//
// One test per named scene. Each test renders the ACTUAL production UI drawing
// code into the headless 240x240 canvas, then gates the captured framebuffer
// against a checked-in golden BMP (exact byte comparison; see bmp.h). Scenes are
// deterministic: firmware adapters are pinned via the headless control API and
// every input is a fixed literal, so repeated runs are byte-identical.

#include <unity.h>

#include <cstdlib>
#include <cstring>

#include "bmp.h"
#include "core/factory_erase.h"
#include "core/provision_button.h"
#include "core/radar_data_state.h"
#include "hardware/display.h"
#include "headless_adapters.h"
#include "services/adsb_types.h"
#include "ui/radar_display.h"
#include "ui/status_screens.h"

namespace {

using services::adsb::Aircraft;
using services::adsb::SnapshotView;

// Build a fixed aircraft record. Offsets are in decimal degrees from the radar
// center so scenes place traffic deterministically.
Aircraft makeAircraft(float lat, float lon, float nose, float track, float gs,
                       const char* callsign, const char* type,
                       const char* alt) {
  Aircraft a{};
  a.lat = lat;
  a.lon = lon;
  a.nose_deg = nose;
  a.track_deg = track;
  a.gs_knots = gs;
  std::strncpy(a.callsign, callsign, sizeof(a.callsign) - 1);
  std::strncpy(a.type, type, sizeof(a.type) - 1);
  std::strncpy(a.alt, alt, sizeof(a.alt) - 1);
  return a;
}

ui::RadarDisplayModel radarModel(core::RadarDataMode mode, uint32_t age_seconds,
                                 bool show_aircraft, const Aircraft* aircraft,
                                 uint16_t count, bool wifi_connected,
                                 uint8_t activity_phase) {
  ui::RadarDisplayModel model{};
  model.data.mode = mode;
  model.data.age_seconds = age_seconds;
  model.data.show_aircraft = show_aircraft;
  model.data.settings_revision = 0;
  model.snapshot.aircraft = aircraft;
  model.snapshot.count = count;
  model.snapshot.settings_revision = 0;
  model.wifi_connected = wifi_connected;
  model.activity_phase = activity_phase;
  return model;
}

// A deterministic multi-aircraft field near the Amsterdam radar center. A mix of
// inside-disc traffic (with callsign/type/altitude tags, headings, and speed
// vectors) and far traffic that renders as beyond-ring rim dots.
constexpr double kCenterLat = 52.3676;
constexpr double kCenterLon = 4.9041;

const Aircraft kTraffic[] = {
    makeAircraft(52.3900f, 4.9300f, 45.0f, 50.0f, 420.0f, "KLM1234", "B738",
                 "FL120"),
    makeAircraft(52.3500f, 4.8700f, 210.0f, 200.0f, 250.0f, "TRA55X", "A320",
                 "3500"),
    makeAircraft(52.3800f, 4.8600f, 315.0f, 310.0f, 180.0f, "EZY88", "A319",
                 "2200"),
    makeAircraft(52.3400f, 4.9500f, 90.0f, 95.0f, 300.0f, "DLH9K", "A21N",
                 "FL080"),
    // Far to the north-east: outside the outer ring -> beyond-ring rim dot.
    makeAircraft(52.7000f, 5.4000f, 60.0f, 60.0f, 460.0f, "BAW7", "B77W",
                 "FL350"),
    // Far to the south-west: another rim dot.
    makeAircraft(52.0500f, 4.3000f, 240.0f, 240.0f, 440.0f, "AFR3", "A359",
                 "FL330"),
};
constexpr uint16_t kTrafficCount =
    static_cast<uint16_t>(sizeof(kTraffic) / sizeof(kTraffic[0]));

void beginRadarScene() {
  nativegfx::resetAdapters();
  nativegfx::setLocation(kCenterLat, kCenterLon);
}

// ---- Radar scenes -------------------------------------------------------

void scene_radar_loading() {
  beginRadarScene();
  const ui::RadarDisplayModel m = radarModel(core::RadarDataMode::Loading, 0,
                                             false, nullptr, 0, true, 2);
  ui::radarDisplayDraw(m);
}

void scene_radar_live_empty() {
  beginRadarScene();
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Live, 2, true, nullptr, 0, true, 0);
  ui::radarDisplayDraw(m);
}

void scene_radar_live_traffic() {
  beginRadarScene();
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Live, 3, true, kTraffic, kTrafficCount,
                 true, 0);
  ui::radarDisplayDraw(m);
}

void scene_radar_stale() {
  beginRadarScene();
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Stale, 42, true, kTraffic, kTrafficCount,
                 true, 0);
  ui::radarDisplayDraw(m);
}

void scene_radar_offline() {
  beginRadarScene();
  // Offline hides targets even though the snapshot still carries them.
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Offline, 120, false, kTraffic,
                 kTrafficCount, true, 0);
  ui::radarDisplayDraw(m);
}

void scene_radar_nowifi() {
  beginRadarScene();
  // Live data but the radio dropped: NoWifi badge, targets still shown.
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Live, 4, true, kTraffic, kTrafficCount,
                 false, 0);
  ui::radarDisplayDraw(m);
}

void scene_radar_runways() {
  nativegfx::resetAdapters();
  nativegfx::setLocation(kCenterLat, kCenterLon);
  nativegfx::setRangeIndex(3);      // 25 km preset: EHAM inside range
  nativegfx::setShowRunways(true);
  const ui::RadarDisplayModel m =
      radarModel(core::RadarDataMode::Live, 3, true, nullptr, 0, true, 0);
  ui::radarDisplayDraw(m);
}

// ---- Status / provisioning scenes --------------------------------------

void scene_status_connecting() {
  statusScreenConnectingBegin("HomeNet-2G");
}

void scene_status_portal_preparing() { statusScreenPortalPreparing(); }

void scene_status_portal_credentials() {
  // Dummy, non-secret setup credentials for display only.
  statusScreenPortalCredentials("PlaneRadar-AB12CD", "swiftpanda42", 272);
  statusScreenClearCredentials();
}

void scene_status_candidate_testing() { statusScreenCandidateTesting(); }

void scene_status_candidate_failed() { statusScreenCandidateFailed(); }

void scene_status_credential_fault() { statusScreenCredentialFault(); }

void scene_status_button_configure() {
  statusScreenButtonPrompt(core::ProvisionButtonPrompt::ReleaseToConfigure);
}

void scene_status_button_confirm_erase() {
  statusScreenButtonPrompt(core::ProvisionButtonPrompt::ConfirmHoldToErase);
}

void scene_status_saved_wifi_failed() { statusScreenSavedWifiFailed(); }

void scene_status_factory_erase_incomplete() {
  core::FactoryEraseOutcome outcome{};
  outcome.sta_cleared = true;
  outcome.location_cleared = true;
  outcome.radar_cleared = false;      // one subsystem failed to verify
  outcome.time_floor_cleared = true;
  statusScreenFactoryErase(outcome);
}

void scene_status_factory_erase_ok() {
  core::FactoryEraseOutcome outcome{};
  outcome.sta_cleared = true;
  outcome.location_cleared = true;
  outcome.radar_cleared = true;
  outcome.time_floor_cleared = true;
  statusScreenFactoryErase(outcome);
}

void scene_status_erase_incomplete_persistent() {
  statusScreenEraseIncomplete();
}

void scene_status_settings_save_failed() { statusScreenSettingsSaveFailed(); }

}  // namespace

// ---- Unity tests: one per golden scene ----------------------------------

#define GOLDEN_TEST(name, fn)                            \
  void test_##name(void) {                               \
    fn();                                                \
    TEST_ASSERT_TRUE_MESSAGE(nativegfx::checkGolden(#name), \
                             "golden mismatch: " #name);  \
  }

GOLDEN_TEST(radar_loading, scene_radar_loading)
GOLDEN_TEST(radar_live_empty, scene_radar_live_empty)
GOLDEN_TEST(radar_live_traffic, scene_radar_live_traffic)
GOLDEN_TEST(radar_stale, scene_radar_stale)
GOLDEN_TEST(radar_offline, scene_radar_offline)
GOLDEN_TEST(radar_nowifi, scene_radar_nowifi)
GOLDEN_TEST(radar_runways, scene_radar_runways)
GOLDEN_TEST(status_connecting, scene_status_connecting)
GOLDEN_TEST(status_portal_preparing, scene_status_portal_preparing)
GOLDEN_TEST(status_portal_credentials, scene_status_portal_credentials)
GOLDEN_TEST(status_candidate_testing, scene_status_candidate_testing)
GOLDEN_TEST(status_candidate_failed, scene_status_candidate_failed)
GOLDEN_TEST(status_credential_fault, scene_status_credential_fault)
GOLDEN_TEST(status_button_configure, scene_status_button_configure)
GOLDEN_TEST(status_button_confirm_erase, scene_status_button_confirm_erase)
GOLDEN_TEST(status_saved_wifi_failed, scene_status_saved_wifi_failed)
GOLDEN_TEST(status_factory_erase_incomplete, scene_status_factory_erase_incomplete)
GOLDEN_TEST(status_factory_erase_ok, scene_status_factory_erase_ok)
GOLDEN_TEST(status_erase_incomplete_persistent, scene_status_erase_incomplete_persistent)
GOLDEN_TEST(status_settings_save_failed, scene_status_settings_save_failed)

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  displayInit();
  UNITY_BEGIN();
  RUN_TEST(test_radar_loading);
  RUN_TEST(test_radar_live_empty);
  RUN_TEST(test_radar_live_traffic);
  RUN_TEST(test_radar_stale);
  RUN_TEST(test_radar_offline);
  RUN_TEST(test_radar_nowifi);
  RUN_TEST(test_radar_runways);
  RUN_TEST(test_status_connecting);
  RUN_TEST(test_status_portal_preparing);
  RUN_TEST(test_status_portal_credentials);
  RUN_TEST(test_status_candidate_testing);
  RUN_TEST(test_status_candidate_failed);
  RUN_TEST(test_status_credential_fault);
  RUN_TEST(test_status_button_configure);
  RUN_TEST(test_status_button_confirm_erase);
  RUN_TEST(test_status_saved_wifi_failed);
  RUN_TEST(test_status_factory_erase_incomplete);
  RUN_TEST(test_status_factory_erase_ok);
  RUN_TEST(test_status_erase_incomplete_persistent);
  RUN_TEST(test_status_settings_save_failed);
  const int result = UNITY_END();
  // The global LovyanGFX canvas / loaded-font singletons have cross-translation-
  // unit static-destruction order that is unspecified and can fault on teardown
  // (a harness-only concern: every render + capture above has already completed).
  // Flush and exit with the Unity result WITHOUT running global destructors so
  // the suite's exit code faithfully reflects the test outcome.
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(result);
}
