#pragma once

#include <cstddef>
#include <cstring>

// Pure, Arduino-free copy into a fixed Wi-Fi credential field (esp_wifi_types
// wifi_sta_config_t.ssid[32] / password[64]). Extracted so the tricky maximum-
// length boundary is native-testable without the ESP headers.
//
// It mirrors the Arduino-ESP32 framework's _wifi_strncpy contract exactly: a
// source span that fills (or exceeds) the field occupies ALL dst_cap bytes with
// NO NUL terminator -- the WiFi driver derives the field length via strnlen over
// the fixed array, and a maximal 32-byte SSID must occupy all 32 bytes -- while a
// shorter span is NUL-terminated. The caller is expected to have zeroed the field
// first, so bytes beyond the copied span stay zero.

namespace core {

// Copy min(src_len, dst_cap) bytes of `src` into `dst` (dst_cap bytes wide),
// NUL-terminating only when the copied length is strictly less than dst_cap.
// Returns the number of bytes copied. A null/zero src copies nothing (and still
// terminates when there is room).
inline size_t wifiFieldCopy(unsigned char* dst, size_t dst_cap, const char* src,
                            size_t src_len) {
  if (dst == nullptr || dst_cap == 0) {
    return 0;
  }
  size_t n = (src == nullptr) ? 0 : src_len;
  if (n > dst_cap) {
    n = dst_cap;
  }
  if (n > 0) {
    memcpy(dst, src, n);
  }
  if (n < dst_cap) {
    dst[n] = 0;
  }
  return n;
}

}  // namespace core
