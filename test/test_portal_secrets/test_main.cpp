#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/portal_secrets.h"

using core::EntropySource;
using core::PortalSecrets;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// --- entropy doubles --------------------------------------------------------

bool fillZeros(void*, uint8_t* out, size_t len) {
  std::memset(out, 0x00, len);
  return true;
}
bool fillOnes(void*, uint8_t* out, size_t len) {
  std::memset(out, 0xFF, len);
  return true;
}
bool fillFail(void*, uint8_t*, size_t) { return false; }

// Scripted byte source that fails once exhausted; lets tests control the exact
// entropy and simulate mid-sequence exhaustion.
struct Scripted {
  const uint8_t* data;
  size_t len;
  size_t pos;
};
bool fillScripted(void* ctx, uint8_t* out, size_t len) {
  Scripted* s = static_cast<Scripted*>(ctx);
  if (s->pos + len > s->len) {
    return false;  // exhausted
  }
  std::memcpy(out, s->data + s->pos, len);
  s->pos += len;
  return true;
}

// Per-call constant source: each fill() writes a distinct byte value, so two
// independent draws produce provably different material.
struct Counter {
  uint8_t v;
};
bool fillCounter(void* ctx, uint8_t* out, size_t len) {
  Counter* c = static_cast<Counter*>(ctx);
  std::memset(out, c->v, len);
  c->v = static_cast<uint8_t>(c->v + 1);
  return true;
}

EntropySource src(core::EntropyFillFn fn, void* ctx = nullptr) {
  return EntropySource{fn, ctx};
}

// Independent reference for the 5-bit MSB-first password mapping.
void refPassword(const uint8_t* buf, char* out) {
  const char* A = core::kPortalPasswordAlphabet;
  for (int k = 0; k < 14; ++k) {
    uint32_t idx = 0;
    for (int b = 0; b < 5; ++b) {
      const uint32_t pos = static_cast<uint32_t>(k) * 5U + b;
      const uint32_t bitval = (buf[pos / 8] >> (7 - (pos % 8))) & 1U;
      idx = (idx << 1) | bitval;
    }
    out[k] = A[idx];
  }
  out[14] = '\0';
}

bool allInAlphabet(const char* s) {
  for (const char* c = s; *c; ++c) {
    if (std::strchr(core::kPortalPasswordAlphabet, *c) == nullptr) {
      return false;
    }
  }
  return true;
}

bool allZero(const void* buf, size_t len) {
  const unsigned char* p = static_cast<const unsigned char*>(buf);
  for (size_t i = 0; i < len; ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- SSID -------------------------------------------------------------------

void test_ssid_uses_last_three_mac_bytes_uppercase() {
  const uint8_t mac[6] = {0x00, 0x11, 0x22, 0xAB, 0xCD, 0xEF};
  char ssid[core::kPortalSsidCap + 1];
  TEST_ASSERT_TRUE(core::formatPortalSsid(mac, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_STRING("PlaneRadar-ABCDEF", ssid);
  TEST_ASSERT_TRUE(std::strlen(ssid) <= 32);
}

void test_ssid_hex_pads_and_uppercases() {
  const uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0x0A, 0xBC, 0x0E};
  char ssid[core::kPortalSsidCap + 1];
  TEST_ASSERT_TRUE(core::formatPortalSsid(mac, ssid, sizeof(ssid)));
  TEST_ASSERT_EQUAL_STRING("PlaneRadar-0ABC0E", ssid);
}

void test_ssid_rejects_small_buffer() {
  const uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  char small[10];
  TEST_ASSERT_FALSE(core::formatPortalSsid(mac, small, sizeof(small)));
}

// --- password ---------------------------------------------------------------

void test_password_all_zero_and_all_one_entropy() {
  char pw[core::kPortalPasswordLen + 1];
  TEST_ASSERT_TRUE(core::generatePortalPassword(src(fillZeros), pw, sizeof(pw)));
  TEST_ASSERT_EQUAL_STRING("AAAAAAAAAAAAAA", pw);  // index 0 -> 'A'
  TEST_ASSERT_TRUE(core::generatePortalPassword(src(fillOnes), pw, sizeof(pw)));
  TEST_ASSERT_EQUAL_STRING("99999999999999", pw);  // index 31 -> '9'
}

void test_password_matches_independent_reference() {
  const uint8_t bytes[9] = {0x12, 0x34, 0x56, 0x78, 0x9A,
                            0xBC, 0xDE, 0xF0, 0x55};
  Scripted s{bytes, sizeof(bytes), 0};
  char pw[core::kPortalPasswordLen + 1];
  TEST_ASSERT_TRUE(
      core::generatePortalPassword(src(fillScripted, &s), pw, sizeof(pw)));
  char expected[core::kPortalPasswordLen + 1];
  refPassword(bytes, expected);
  TEST_ASSERT_EQUAL_STRING(expected, pw);
  TEST_ASSERT_EQUAL_UINT(14, std::strlen(pw));
  TEST_ASSERT_TRUE(allInAlphabet(pw));
}

void test_password_entropy_failure_zeroizes_and_fails() {
  char pw[core::kPortalPasswordLen + 1];
  std::memset(pw, 'Z', sizeof(pw));
  TEST_ASSERT_FALSE(core::generatePortalPassword(src(fillFail), pw, sizeof(pw)));
  TEST_ASSERT_TRUE(allZero(pw, sizeof(pw)));  // no weak fallback; output wiped
}

void test_password_rejects_small_buffer() {
  char small[10];
  TEST_ASSERT_FALSE(core::generatePortalPassword(src(fillZeros), small,
                                                 sizeof(small)));
}

// --- CSRF token -------------------------------------------------------------

void test_csrf_hex_encoding() {
  char tok[core::kPortalCsrfTokenLen + 1];
  TEST_ASSERT_TRUE(core::generateCsrfToken(src(fillZeros), tok, sizeof(tok)));
  TEST_ASSERT_EQUAL_STRING("00000000000000000000000000000000", tok);
  TEST_ASSERT_TRUE(core::generateCsrfToken(src(fillOnes), tok, sizeof(tok)));
  TEST_ASSERT_EQUAL_STRING("ffffffffffffffffffffffffffffffff", tok);
  TEST_ASSERT_EQUAL_UINT(32, std::strlen(tok));
}

void test_csrf_entropy_failure_zeroizes_and_fails() {
  char tok[core::kPortalCsrfTokenLen + 1];
  std::memset(tok, 'Z', sizeof(tok));
  TEST_ASSERT_FALSE(core::generateCsrfToken(src(fillFail), tok, sizeof(tok)));
  TEST_ASSERT_TRUE(allZero(tok, sizeof(tok)));
}

// --- constant-time compare --------------------------------------------------

void test_csrf_compare_full_length() {
  const char* a = "0123456789abcdef0123456789abcdef";
  char b[33];
  std::strcpy(b, a);
  TEST_ASSERT_TRUE(core::csrfTokenEqual(a, b, 32));
  b[0] = 'X';  // first-byte difference
  TEST_ASSERT_FALSE(core::csrfTokenEqual(a, b, 32));
  std::strcpy(b, a);
  b[31] = 'X';  // last-byte difference must still be detected (full length)
  TEST_ASSERT_FALSE(core::csrfTokenEqual(a, b, 32));
}

void test_csrf_compare_zero_length_is_false() {
  // A zero-length compare can never authenticate: fail closed even for two
  // identical (empty) operands, so an empty issued token is unusable.
  TEST_ASSERT_FALSE(core::csrfTokenEqual("", "", 0));
  TEST_ASSERT_FALSE(core::csrfTokenEqual("abc", "abc", 0));
}

// --- secure zero ------------------------------------------------------------

void test_secure_zero_wipes_buffer() {
  unsigned char buf[24];
  std::memset(buf, 0xAB, sizeof(buf));
  core::secureZero(buf, sizeof(buf));
  TEST_ASSERT_TRUE(allZero(buf, sizeof(buf)));
}

// --- bundle -----------------------------------------------------------------

void test_secrets_bundle_success_uses_independent_draws() {
  const uint8_t mac[6] = {0, 1, 2, 3, 4, 5};
  Counter c{1};  // password draw -> 0x01 bytes, csrf draw -> 0x02 bytes
  PortalSecrets s;
  TEST_ASSERT_TRUE(core::generatePortalSecrets(mac, src(fillCounter, &c), &s));
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL_STRING("PlaneRadar-030405", s.ssid);
  TEST_ASSERT_EQUAL_UINT(14, std::strlen(s.password));
  TEST_ASSERT_TRUE(allInAlphabet(s.password));
  // The CSRF token came from the SECOND, independent draw (0x02 bytes).
  TEST_ASSERT_EQUAL_STRING("02020202020202020202020202020202", s.csrf);
}

void test_secrets_bundle_fails_and_zeroizes_on_entropy_exhaustion() {
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  // Provide exactly 9 bytes: enough for the password draw, not the CSRF draw.
  uint8_t nine[9];
  std::memset(nine, 0x33, sizeof(nine));
  Scripted s{nine, sizeof(nine), 0};
  PortalSecrets out;
  std::memset(&out, 0x5A, sizeof(out));
  TEST_ASSERT_FALSE(
      core::generatePortalSecrets(mac, src(fillScripted, &s), &out));
  TEST_ASSERT_FALSE(out.valid);
  TEST_ASSERT_TRUE(allZero(out.password, sizeof(out.password)));
  TEST_ASSERT_TRUE(allZero(out.csrf, sizeof(out.csrf)));
}

void test_zero_portal_secrets() {
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  PortalSecrets s;
  TEST_ASSERT_TRUE(core::generatePortalSecrets(mac, src(fillOnes), &s));
  core::zeroPortalSecrets(&s);
  TEST_ASSERT_FALSE(s.valid);
  TEST_ASSERT_TRUE(allZero(s.password, sizeof(s.password)));
  TEST_ASSERT_TRUE(allZero(s.csrf, sizeof(s.csrf)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ssid_uses_last_three_mac_bytes_uppercase);
  RUN_TEST(test_ssid_hex_pads_and_uppercases);
  RUN_TEST(test_ssid_rejects_small_buffer);
  RUN_TEST(test_password_all_zero_and_all_one_entropy);
  RUN_TEST(test_password_matches_independent_reference);
  RUN_TEST(test_password_entropy_failure_zeroizes_and_fails);
  RUN_TEST(test_password_rejects_small_buffer);
  RUN_TEST(test_csrf_hex_encoding);
  RUN_TEST(test_csrf_entropy_failure_zeroizes_and_fails);
  RUN_TEST(test_csrf_compare_full_length);
  RUN_TEST(test_csrf_compare_zero_length_is_false);
  RUN_TEST(test_secure_zero_wipes_buffer);
  RUN_TEST(test_secrets_bundle_success_uses_independent_draws);
  RUN_TEST(test_secrets_bundle_fails_and_zeroizes_on_entropy_exhaustion);
  RUN_TEST(test_zero_portal_secrets);
  return UNITY_END();
}
