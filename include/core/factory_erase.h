#pragma once

// Truthful factory-erase result aggregation (pure, Arduino-free).
//
// A factory erase clears several independent persisted subsystems (STA flash
// credentials, radar location, radar preferences, and the trusted-time floor).
// Each step is performed AND read back by the ESP adapter, which records whether
// that subsystem verifiably reached a cleared state. This pure helper decides
// whether the device may honestly claim a full wipe: it may say "Erased" ONLY
// when every step verifiably cleared, otherwise it must show an incomplete /
// failure screen. Extracted so the honest-reporting contract is native-testable
// without any NVS/Wi-Fi hardware.

namespace core {

// Per-subsystem cleared flags. Each is true ONLY when that subsystem's cleared
// state was confirmed by a read-back (never optimistically true because a call
// "returned"). The adapter fills these in after erasing each subsystem.
struct FactoryEraseOutcome {
  bool sta_cleared;         // STA credential flash erased AND read back empty
  bool location_cleared;    // radar location NVS keys removed AND absent
  bool radar_cleared;       // radar range/units/runway prefs removed AND absent
  bool time_floor_cleared;  // persisted trusted-time floor namespace cleared
};

// True only when EVERY step verifiably cleared. Any false flag means the device
// must NOT claim a clean wipe -- it shows a truthful incomplete/failure screen so
// the user can retry rather than trusting a partial erase.
inline constexpr bool factoryEraseAllCleared(const FactoryEraseOutcome& o) {
  return o.sta_cleared && o.location_cleared && o.radar_cleared &&
         o.time_floor_cleared;
}

}  // namespace core
