#include "services/settings_events.h"

#include "core/settings_events.h"

namespace services::settings {

namespace {

// Single owner of the runtime settings revision, shared by the location save
// callback and the radar-range control and read by the main loop.
core::SettingsState s_state{};

}  // namespace

void markQueryChanged() { core::settingsQueryChanged(&s_state); }

bool consumeQueryChange() { return core::settingsConsumeQueryChange(&s_state); }

void markVisualChanged() { core::settingsVisualChanged(&s_state); }

bool consumeVisualChange() { return core::settingsConsumeVisualChange(&s_state); }

uint32_t revision() { return core::settingsRevision(s_state); }

}  // namespace services::settings
