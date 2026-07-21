#include "core/portal_secrets.h"

#include <cstring>

namespace core {

namespace {

char hexLower(uint8_t nibble) {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

char hexUpper(uint8_t nibble) {
  return static_cast<char>(nibble < 10 ? '0' + nibble : 'A' + (nibble - 10));
}

// Extract the 5-bit group starting at bit offset `bit` (MSB-first) from buf.
// Reads at most two bytes and never past `bytes`, so the caller sizes buf to
// hold ceil((count*5)/8) bytes with the final partial group fully covered.
uint8_t extract5(const uint8_t* buf, size_t bytes, uint32_t bit) {
  const uint32_t byte0 = bit >> 3;
  const uint32_t off = bit & 7U;
  uint32_t window = static_cast<uint32_t>(buf[byte0]) << 8;
  if (byte0 + 1U < bytes) {
    window |= buf[byte0 + 1U];
  }
  return static_cast<uint8_t>((window >> (11U - off)) & 0x1FU);
}

}  // namespace

void secureZero(void* buf, size_t len) {
  if (buf == nullptr) {
    return;
  }
  volatile unsigned char* p = static_cast<volatile unsigned char*>(buf);
  while (len-- > 0) {
    *p++ = 0U;
  }
}

bool formatPortalSsid(const uint8_t mac[6], char* out, size_t out_cap) {
  const size_t prefix_len = std::strlen(kPortalSsidPrefix);  // 11
  const size_t need = prefix_len + 6U + 1U;                  // + 3 bytes hex + NUL
  if (out == nullptr || mac == nullptr || out_cap < need) {
    return false;
  }
  std::memcpy(out, kPortalSsidPrefix, prefix_len);
  size_t o = prefix_len;
  for (int i = 3; i < 6; ++i) {
    out[o++] = hexUpper(static_cast<uint8_t>(mac[i] >> 4));
    out[o++] = hexUpper(static_cast<uint8_t>(mac[i] & 0x0F));
  }
  out[o] = '\0';
  return true;
}

bool generatePortalPassword(const EntropySource& entropy, char* out,
                            size_t out_cap) {
  if (out == nullptr || out_cap < static_cast<size_t>(kPortalPasswordLen) + 1U) {
    return false;
  }
  // 14 * 5 = 70 bits; request 9 bytes (72 bits) and use 70.
  uint8_t buf[9];
  if (entropy.fill == nullptr ||
      !entropy.fill(entropy.ctx, buf, sizeof(buf))) {
    secureZero(out, out_cap);
    secureZero(buf, sizeof(buf));
    return false;
  }
  for (uint16_t k = 0; k < kPortalPasswordLen; ++k) {
    const uint8_t idx = extract5(buf, sizeof(buf), static_cast<uint32_t>(k) * 5U);
    out[k] = kPortalPasswordAlphabet[idx];  // 0..31 -> alphabet, no modulo bias
  }
  out[kPortalPasswordLen] = '\0';
  secureZero(buf, sizeof(buf));
  return true;
}

bool generateCsrfToken(const EntropySource& entropy, char* out, size_t out_cap) {
  if (out == nullptr ||
      out_cap < static_cast<size_t>(kPortalCsrfTokenLen) + 1U) {
    return false;
  }
  uint8_t raw[kPortalCsrfTokenBytes];
  if (entropy.fill == nullptr ||
      !entropy.fill(entropy.ctx, raw, sizeof(raw))) {
    secureZero(out, out_cap);
    secureZero(raw, sizeof(raw));
    return false;
  }
  for (uint16_t i = 0; i < kPortalCsrfTokenBytes; ++i) {
    out[i * 2] = hexLower(static_cast<uint8_t>(raw[i] >> 4));
    out[i * 2 + 1] = hexLower(static_cast<uint8_t>(raw[i] & 0x0F));
  }
  out[kPortalCsrfTokenLen] = '\0';
  secureZero(raw, sizeof(raw));
  return true;
}

bool csrfTokenEqual(const char* a, const char* b, size_t len) {
  if (a == nullptr || b == nullptr || len == 0U) {
    return false;  // a zero-length compare can never authenticate (fail closed)
  }
  uint8_t diff = 0U;
  for (size_t i = 0; i < len; ++i) {
    diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
  }
  return diff == 0U;
}

bool generatePortalSecrets(const uint8_t mac[6], const EntropySource& entropy,
                           PortalSecrets* out) {
  if (out == nullptr) {
    return false;
  }
  std::memset(out, 0, sizeof(*out));
  if (!formatPortalSsid(mac, out->ssid, sizeof(out->ssid)) ||
      !generatePortalPassword(entropy, out->password, sizeof(out->password)) ||
      !generateCsrfToken(entropy, out->csrf, sizeof(out->csrf))) {
    zeroPortalSecrets(out);
    return false;
  }
  out->valid = true;
  return true;
}

void zeroPortalSecrets(PortalSecrets* secrets) {
  if (secrets == nullptr) {
    return;
  }
  secureZero(secrets, sizeof(*secrets));
  secrets->valid = false;
}

}  // namespace core
