#include "ui/status_screens.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

#include "config.h"
#include "core/provision_button.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace plane_radar_fonts = lgfx::v1::fonts;

namespace {

constexpr int kLineGap = 6;
const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

constexpr int kSpinnerDotCount = 10;
constexpr int kSpinnerRadius = 113;
constexpr int kSpinnerDotRadius = 2;
constexpr int kSpinnerEraseRadius = 4;
constexpr float kSpinnerStepDeg = 6.0f;

struct SpinnerDot {
  int x = 0;
  int y = 0;
  bool drawn = false;
};

char s_connecting_ssid[33];
char s_ssid_line[33];
constexpr int kConnectingTextMaxWidthPx = 220;

// Static text buffers for the credentials screen, at file scope so they can be
// securely wiped when the session leaves the credentials screen (the WPA2
// password rendered here must not linger in status RAM).
char s_cred_ssid_line[40];
char s_cred_pass_line[40];
char s_cred_count_line[24];
float s_spinner_angle_deg = -90.0f;
SpinnerDot s_spinner_dots[kSpinnerDotCount];
bool s_connecting_text_drawn = false;

constexpr auto& kGfxTitle = plane_radar_fonts::FreeSans18pt7b;
constexpr auto& kGfxBody = plane_radar_fonts::FreeSans12pt7b;
constexpr auto& kGfxDetail = plane_radar_fonts::Font2;
constexpr auto& kPortalGfxTitle = plane_radar_fonts::FreeSansBold18pt7b;
constexpr auto& kPortalGfxBody = plane_radar_fonts::FreeSansBold12pt7b;
constexpr auto& kPortalGfxEmphasis = plane_radar_fonts::FreeSansBold18pt7b;
constexpr auto& kConnectingGfxDetail = plane_radar_fonts::FreeSans9pt7b;

struct TextLine {
  const char* text;
  float vlw_size;
  const lgfx::GFXfont* gfx_font;
};

int lineHeightGfx(const lgfx::GFXfont* font) {
  displayFontSetBitmap(tft, font);
  return tft.fontHeight();
}

int lineHeightVlw(float size) {
  displayFontSetSmoothSize(tft, size);
  return tft.fontHeight();
}

void applyLineStyle(const TextLine& line) {
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, line.vlw_size);
  } else {
    displayFontSetBitmap(tft, line.gfx_font);
  }
}

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(textdatum_t::middle_center);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    if (displayFontIsSmooth()) {
      total_h += lineHeightVlw(lines[i].vlw_size);
    } else {
      total_h += lineHeightGfx(lines[i].gfx_font);
    }
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    applyLineStyle(lines[i]);
    const int h =
        displayFontIsSmooth() ? lineHeightVlw(lines[i].vlw_size)
                              : lineHeightGfx(lines[i].gfx_font);
    tft.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
}

constexpr float kConnectingDetailVlw = 0.92f;

void applyConnectingDetailStyle() {
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, kConnectingDetailVlw);
  } else {
    displayFontSetBitmap(tft, &kConnectingGfxDetail);
  }
}

/** SSID on one line; truncate with … if wider than kConnectingTextMaxWidthPx. */
void fitSsidLine() {
  strncpy(s_ssid_line, s_connecting_ssid, sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
  applyConnectingDetailStyle();
  if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
    return;
  }
  const size_t len = strlen(s_connecting_ssid);
  for (size_t n = len; n > 0; --n) {
    snprintf(s_ssid_line, sizeof(s_ssid_line), "%.*s…", static_cast<int>(n),
             s_connecting_ssid);
    if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
      return;
    }
  }
  strncpy(s_ssid_line, "…", sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
}

void drawConnectingText() {
  tft.fillScreen(config::kColorBlack);

  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);

  applyConnectingDetailStyle();
  const int detail_h = tft.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  const int block_top = (config::kDisplayHeight - total_h) / 2;
  constexpr int kPanelPadY = 8;
  tft.fillRect(kCenterX - kConnectingTextMaxWidthPx / 2, block_top - kPanelPadY,
               kConnectingTextMaxWidthPx, total_h + kPanelPadY * 2, config::kColorBlack);

  int y = block_top;
  tft.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  tft.drawString(s_ssid_line, kCenterX, y + detail_h / 2);

  s_connecting_text_drawn = true;
}

void eraseSpinnerDots() {
  for (int i = 0; i < kSpinnerDotCount; ++i) {
    if (!s_spinner_dots[i].drawn) {
      continue;
    }
    tft.fillCircle(s_spinner_dots[i].x, s_spinner_dots[i].y, kSpinnerEraseRadius,
                   config::kColorBlack);
    s_spinner_dots[i].drawn = false;
  }
}

void drawSpinnerDots() {
  constexpr float kDegToRad = 0.01745329252f;
  const float head_rad = s_spinner_angle_deg * kDegToRad;

  for (int i = 0; i < kSpinnerDotCount; ++i) {
    const float a = head_rad - static_cast<float>(i) * (6.283185307f / kSpinnerDotCount);
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));

    const int fade = 255 - i * 22;
    const uint16_t color = tft.color565(0, fade, 0);
    tft.fillSmoothCircle(x, y, kSpinnerDotRadius, color);

    s_spinner_dots[i].x = x;
    s_spinner_dots[i].y = y;
    s_spinner_dots[i].drawn = true;
  }
}

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  s_spinner_angle_deg = -90.0f;
  for (auto& dot : s_spinner_dots) {
    dot.drawn = false;
  }
  s_connecting_text_drawn = false;
  drawConnectingText();
  drawSpinnerDots();
}

void statusScreenConnectingTick() {
  if (!s_connecting_text_drawn) {
    drawConnectingText();
  }
  eraseSpinnerDots();
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
  drawSpinnerDots();
}

void statusScreenPortalPreparing() {
  const TextLine lines[] = {
      {"Wi-Fi setup", 1.15f, &kPortalGfxTitle},
      {"Preparing secure", 1.05f, &kPortalGfxBody},
      {"network...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenPortalCredentials(const char* ssid, const char* password,
                                   uint32_t seconds_left) {
  snprintf(s_cred_ssid_line, sizeof(s_cred_ssid_line), "%s",
           (ssid != nullptr && ssid[0] != '\0') ? ssid : "PlaneRadar");
  snprintf(s_cred_pass_line, sizeof(s_cred_pass_line), "%s",
           (password != nullptr) ? password : "");
  const uint32_t mins = seconds_left / 60U;
  const uint32_t secs = seconds_left % 60U;
  snprintf(s_cred_count_line, sizeof(s_cred_count_line), "Closes in %lu:%02lu",
           static_cast<unsigned long>(mins), static_cast<unsigned long>(secs));
  const TextLine lines[] = {
      {"Network:", 1.0f, &kPortalGfxBody},
      {s_cred_ssid_line, 1.08f, &kPortalGfxEmphasis},
      {"Password:", 1.0f, &kPortalGfxBody},
      {s_cred_pass_line, 1.08f, &kPortalGfxEmphasis},
      {"Open 192.168.4.1", 1.0f, &kPortalGfxBody},
      {s_cred_count_line, 1.0f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenClearCredentials() {
  // Volatile-safe wipe so the compiler cannot elide it as a dead store.
  volatile char* p = s_cred_pass_line;
  for (size_t i = 0; i < sizeof(s_cred_pass_line); ++i) {
    p[i] = 0;
  }
  memset(s_cred_ssid_line, 0, sizeof(s_cred_ssid_line));
  memset(s_cred_count_line, 0, sizeof(s_cred_count_line));
}

void statusScreenCandidateTesting() {
  const TextLine lines[] = {
      {"Testing Wi-Fi", 1.15f, &kPortalGfxTitle},
      {"Connecting to", 1.05f, &kPortalGfxBody},
      {"your network...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenCandidateFailed() {
  const TextLine lines[] = {
      {"Wi-Fi failed", 1.15f, &kGfxTitle},
      {"Reopening setup", 1.0f, &kGfxBody},
      {"with same name", 1.0f, &kGfxBody},
      {"and password.", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenCommitting() {
  const TextLine lines[] = {
      {"Saving Wi-Fi", 1.15f, &kPortalGfxTitle},
      {"Please wait...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenSavedWifiFailed() {
  const TextLine lines[] = {
      {"Could not connect", 1.12f, &kGfxTitle},
      {"Check Wi-Fi password", 1.0f, &kGfxBody},
      {"Hold BOOT 2-8 sec,", 1.0f, &kGfxBody},
      {"release to set up", 1.0f, &kGfxBody},
      {"new Wi-Fi", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenCredentialFault() {
  // Truthful generic wording: the write AND its rollback both failed to verify,
  // so the stored credential's state is unknown -- never claim it "could not be
  // saved" (it may have been saved, or a good one may be gone; we cannot tell).
  const TextLine lines[] = {
      {"Wi-Fi fault", 1.12f, &kGfxTitle},
      {"Wi-Fi credential", 1.0f, &kGfxBody},
      {"state unknown", 1.0f, &kGfxBody},
      {"Hold BOOT 8s,", 1.0f, &kGfxBody},
      {"release, then", 1.0f, &kGfxBody},
      {"hold 3s to erase", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenEraseIncomplete() {
  const TextLine lines[] = {
      {"Not fully erased", 1.12f, &kGfxTitle},
      {"Some settings may", 1.0f, &kGfxBody},
      {"remain. Hold BOOT", 1.0f, &kGfxBody},
      {"8s, release, then", 1.0f, &kGfxBody},
      {"hold 3s to retry", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenSettingsSaveFailed() {
  const TextLine lines[] = {
      {"Wi-Fi saved", 1.12f, &kGfxTitle},
      {"Settings save", 1.0f, &kGfxBody},
      {"failed - location,", 1.0f, &kGfxBody},
      {"units, or runways", 1.0f, &kGfxBody},
      {"may be unchanged", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenButtonPrompt(core::ProvisionButtonPrompt prompt) {
  switch (prompt) {
    case core::ProvisionButtonPrompt::ReleaseToConfigure: {
      // Holding into the arm window only ARMS erase; it never erases on its
      // own -- a second, separate hold is always required (see EraseArmedRelease).
      const TextLine lines[] = {
          {"Release now to", 1.12f, &kPortalGfxTitle},
          {"configure Wi-Fi", 1.05f, &kPortalGfxBody},
          {"Keep holding to", 1.0f, &kPortalGfxBody},
          {"arm erase (8s)", 1.0f, &kPortalGfxBody},
      };
      drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                    sizeof(lines) / sizeof(lines[0]));
      break;
    }
    case core::ProvisionButtonPrompt::EraseArmedRelease: {
      const TextLine lines[] = {
          {"Erase armed", 1.12f, &kGfxTitle},
          {"Release, then", 1.0f, &kGfxBody},
          {"hold 3s again", 1.0f, &kGfxBody},
          {"within 10s", 1.0f, &kGfxBody},
      };
      drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                    sizeof(lines) / sizeof(lines[0]));
      break;
    }
    case core::ProvisionButtonPrompt::ConfirmHoldToErase: {
      const TextLine lines[] = {
          {"Confirm erase", 1.12f, &kGfxTitle},
          {"Press & hold 3s", 1.0f, &kGfxBody},
          {"(within 10s)", 1.0f, &kGfxBody},
      };
      drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                    sizeof(lines) / sizeof(lines[0]));
      break;
    }
    case core::ProvisionButtonPrompt::KeepHoldingToErase: {
      const TextLine lines[] = {
          {"Keep holding", 1.12f, &kGfxTitle},
          {"3 sec to erase", 1.05f, &kGfxBody},
          {"all settings...", 1.0f, &kGfxBody},
      };
      drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                    sizeof(lines) / sizeof(lines[0]));
      break;
    }
    case core::ProvisionButtonPrompt::Cancelled:
    case core::ProvisionButtonPrompt::None:
    default:
      break;
  }
}

void statusScreenFactoryErase(const core::FactoryEraseOutcome& outcome) {
  if (core::factoryEraseAllCleared(outcome)) {
    const TextLine lines[] = {
        {"Erased", 1.15f, &kPortalGfxTitle},
        {"Wi-Fi, location,", 1.05f, &kPortalGfxBody},
        {"units cleared.", 1.05f, &kPortalGfxBody},
        {"Restarting...", 1.0f, &kPortalGfxBody},
    };
    drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                  sizeof(lines) / sizeof(lines[0]));
  } else {
    // Truthful incomplete wipe: name it plainly and restart so the user can
    // retry. NEVER claim a clean erase when any subsystem did not verify. Avoid
    // implying a single hold retries -- the full two-stage gesture (hold 8s,
    // release, hold 3s again) is spelled out on statusScreenEraseIncomplete if
    // the fault persists after restart.
    const TextLine lines[] = {
        {"Not fully erased", 1.12f, &kGfxTitle},
        {"Some settings may", 1.0f, &kGfxBody},
        {"remain. Restarting", 1.0f, &kGfxBody},
        {"to retry erase.", 1.0f, &kGfxBody},
    };
    drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                  sizeof(lines) / sizeof(lines[0]));
  }
}
