#include "services/device_identity.h"

#include <esp_mac.h>
#include <esp_random.h>

namespace services::identity {

namespace {

// EntropyFillFn bridge to the hardware RNG. esp_fill_random() draws from the
// hardware entropy pool (cryptographically strong while WiFi/BT RF is active, per
// the ESP-IDF docs), so the portal keeps the STA radio up before requesting
// secrets. It writes exactly `len` bytes and returns void, so the only failure
// this bridge reports is a malformed request; it NEVER logs the bytes.
bool espEntropyFill(void* /*ctx*/, uint8_t* out, size_t len) {
  if (out == nullptr) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  esp_fill_random(out, len);
  return true;
}

}  // namespace

bool factoryMac(uint8_t out[6]) {
  if (out == nullptr) {
    return false;
  }
  return esp_read_mac(out, ESP_MAC_WIFI_STA) == ESP_OK;
}

bool softApMac(uint8_t out[6]) {
  if (out == nullptr) {
    return false;
  }
  return esp_read_mac(out, ESP_MAC_WIFI_SOFTAP) == ESP_OK;
}

core::EntropySource entropySource() {
  return core::EntropySource{&espEntropyFill, nullptr};
}

}  // namespace services::identity
