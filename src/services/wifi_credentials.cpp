#include "services/wifi_credentials.h"

#include <cstring>

#include <esp_wifi.h>

#include "core/portal_secrets.h"  // core::secureZero
#include "core/wifi_field.h"      // core::wifiFieldCopy (pure, native-tested)

namespace services::wifi_creds {

namespace {

// Thin wrapper over the pure, native-tested field copy (mirrors _wifi_strncpy).
size_t copyField(uint8_t* dst, size_t dst_cap, const char* src, size_t src_len) {
  return core::wifiFieldCopy(dst, dst_cap, src, src_len);
}

}  // namespace

bool snapshotSta(wifi_config_t* out) { return getSta(out); }

bool buildSta(wifi_config_t* out, const char* ssid, size_t ssid_len,
              const char* psk, size_t psk_len) {
  if (out == nullptr) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (ssid_len > kSsidMax || psk_len > kPskMax) {
    return false;
  }
  // Match Arduino WiFiSTA defaults so a candidate connects the same way the
  // committed credential later will.
  out->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  out->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  out->sta.threshold.rssi = -127;
  out->sta.threshold.authmode = WIFI_AUTH_OPEN;
  out->sta.pmf_cfg.capable = true;
  out->sta.pmf_cfg.required = false;
  out->sta.bssid_set = 0;

  copyField(out->sta.ssid, sizeof(out->sta.ssid), ssid, ssid_len);
  if (psk_len > 0) {
    copyField(out->sta.password, sizeof(out->sta.password), psk, psk_len);
    out->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  }
  return true;
}

bool buildAp(wifi_config_t* out, const char* ssid, size_t ssid_len,
             const char* psk, size_t psk_len, uint8_t channel, uint8_t max_conn,
             bool hidden) {
  if (out == nullptr) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  // An empty/oversized SSID, or a passphrase outside the WPA2 8..63 range, would
  // make the AP fail to start or (worse) come up OPEN. Reject rather than expose
  // an unsecured setup network.
  if (ssid_len == 0 || ssid_len > kSsidMax || psk_len < 8 || psk_len > kPskMax) {
    return false;
  }
  const size_t ssid_copied =
      copyField(out->ap.ssid, sizeof(out->ap.ssid), ssid, ssid_len);
  copyField(out->ap.password, sizeof(out->ap.password), psk, psk_len);
  out->ap.ssid_len = static_cast<uint8_t>(ssid_copied);
  out->ap.channel = channel;
  out->ap.authmode = WIFI_AUTH_WPA2_PSK;         // explicit WPA2-PSK, never OPEN
  out->ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;  // AES-CCMP only (no TKIP)
  out->ap.ssid_hidden = hidden ? 1 : 0;          // hidden==false => broadcast
  out->ap.max_connection = max_conn;
  out->ap.beacon_interval = 100;
  return true;
}

bool sameCredentials(const wifi_config_t& a, const wifi_config_t& b) {
  return memcmp(a.sta.ssid, b.sta.ssid, sizeof(a.sta.ssid)) == 0 &&
         memcmp(a.sta.password, b.sta.password, sizeof(a.sta.password)) == 0;
}

bool hasSsid(const wifi_config_t& c) { return c.sta.ssid[0] != 0; }

void copySsid(const wifi_config_t& c, char* out, size_t out_cap) {
  if (out == nullptr || out_cap == 0) {
    return;
  }
  size_t n = strnlen(reinterpret_cast<const char*>(c.sta.ssid),
                     sizeof(c.sta.ssid));
  if (n > out_cap - 1) {
    n = out_cap - 1;
  }
  memcpy(out, c.sta.ssid, n);
  out[n] = 0;
}

bool ssidEquals(const wifi_config_t& c, const char* ssid, size_t ssid_len) {
  if (ssid == nullptr) {
    return false;
  }
  const size_t have = strnlen(reinterpret_cast<const char*>(c.sta.ssid),
                              sizeof(c.sta.ssid));
  return have == ssid_len && memcmp(c.sta.ssid, ssid, ssid_len) == 0;
}

void inheritPassword(const wifi_config_t& src, wifi_config_t* dst) {
  if (dst == nullptr) {
    return;
  }
  memcpy(dst->sta.password, src.sta.password, sizeof(dst->sta.password));
  dst->sta.threshold.authmode = src.sta.threshold.authmode;
}

void zeroize(wifi_config_t* c) {
  if (c == nullptr) {
    return;
  }
  core::secureZero(c, sizeof(*c));
}

bool setStorageRam() {
  return esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK;
}

bool setStorageFlash() {
  return esp_wifi_set_storage(WIFI_STORAGE_FLASH) == ESP_OK;
}

bool applySta(const wifi_config_t& c) {
  // esp_wifi_set_config takes a non-const pointer but does not mutate the config.
  wifi_config_t tmp = c;
  const bool ok = esp_wifi_set_config(WIFI_IF_STA, &tmp) == ESP_OK;
  core::secureZero(&tmp, sizeof(tmp));  // wipe the credential-bearing temp copy
  return ok;
}

bool getSta(wifi_config_t* out) {
  if (out == nullptr) {
    return false;
  }
  return esp_wifi_get_config(WIFI_IF_STA, out) == ESP_OK;
}

bool eraseSta() {
  if (!setStorageFlash()) {
    setStorageRam();  // always leave runtime storage in RAM
    return false;
  }
  wifi_config_t empty;
  memset(&empty, 0, sizeof(empty));
  bool ok = esp_wifi_set_config(WIFI_IF_STA, &empty) == ESP_OK;
  if (ok) {
    // Read the flash STA config back and confirm it is genuinely empty, so the
    // caller never reports a successful wipe on a silently-failed erase.
    wifi_config_t readback;
    ok = getSta(&readback) && !hasSsid(readback);
    core::secureZero(&readback, sizeof(readback));
  }
  setStorageRam();  // return storage selection to RAM regardless of outcome
  return ok;
}

}  // namespace services::wifi_creds
