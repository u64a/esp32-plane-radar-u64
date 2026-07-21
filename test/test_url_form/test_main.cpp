#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/url_form.h"

using core::FormParseResult;
using core::ProvisioningForm;
using core::ProvisioningValidity;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int P(FormParseResult r) { return static_cast<int>(r); }
int V(ProvisioningValidity r) { return static_cast<int>(r); }

FormParseResult parseBody(const char* body, ProvisioningForm* out,
                          uint16_t max_pairs = 12) {
  return core::urlFormParse(body, static_cast<uint16_t>(std::strlen(body)),
                            max_pairs, out);
}

// Build a fully valid provisioning form for semantic tests, then let callers
// tweak individual fields.
void makeValid(ProvisioningForm* f) {
  TEST_ASSERT_EQUAL_INT(
      P(FormParseResult::Ok),
      P(parseBody("csrf=0123456789abcdef0123456789abcdef&ssid=HomeNet&"
                  "psk=password1&lat=52.37&lon=4.90",
                  f)));
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- decoding ---------------------------------------------------------------

void test_basic_known_fields() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("csrf=abc&ssid=Home&psk=secret12", &f)));
  TEST_ASSERT_TRUE(f.csrf.present);
  TEST_ASSERT_EQUAL_STRING("abc", f.csrf.value);
  TEST_ASSERT_TRUE(f.ssid.present);
  TEST_ASSERT_EQUAL_STRING("Home", f.ssid.value);
  TEST_ASSERT_TRUE(f.psk.present);
  TEST_ASSERT_EQUAL_STRING("secret12", f.psk.value);
  TEST_ASSERT_FALSE(f.lat.present);
}

void test_percent_and_plus_decoding() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("ssid=My%20Net&psk=a%2Bb+c", &f)));
  TEST_ASSERT_EQUAL_STRING("My Net", f.ssid.value);
  TEST_ASSERT_EQUAL_STRING("a+b c", f.psk.value);  // %2B -> '+', '+' -> ' '
}

void test_malformed_percent_rejected() {
  ProvisioningForm f;
  const char* bads[] = {"ssid=%2", "ssid=%", "ssid=%zz", "ssid=a%2g",
                        "ssid=%2X"};
  for (const char* b : bads) {
    TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding), P(parseBody(b, &f)));
  }
}

void test_decoded_control_or_nul_rejected() {
  ProvisioningForm f;
  const char* bads[] = {"ssid=%00", "ssid=%0a", "ssid=%1f", "ssid=%7f"};
  for (const char* b : bads) {
    TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding), P(parseBody(b, &f)));
  }
}

void test_raw_control_byte_rejected() {
  ProvisioningForm f;
  const char body[] = {'s', 's', 'i', 'd', '=', 'a', 0x01, 'b'};
  TEST_ASSERT_EQUAL_INT(
      P(FormParseResult::BadEncoding),
      P(core::urlFormParse(body, sizeof(body), 12, &f)));
}

// A decoded byte >= 0x7f (DEL and every high byte, e.g. the UTF-8 bytes of
// non-ASCII text) violates the printable-ASCII contract and is rejected, exactly
// like the raw equivalent byte.
void test_encoded_high_bytes_rejected() {
  ProvisioningForm f;
  const char* bads[] = {"ssid=%80", "ssid=%C3%A9", "ssid=%ff",
                        "ssid=%FE",  "psk=ab%E9cd", "ssid=%7f"};
  for (const char* b : bads) {
    TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding), P(parseBody(b, &f)));
  }
}

void test_raw_high_byte_rejected() {
  ProvisioningForm f;
  const char body[] = {'s', 's', 'i', 'd', '=', 'a',
                       static_cast<char>(0xC3), 'b'};
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding),
                        P(core::urlFormParse(body, sizeof(body), 12, &f)));
}

// The raw byte and its percent-encoding are treated identically: for a printable
// byte both decode to the same value; for a high byte both are rejected.
void test_raw_and_encoded_equivalence() {
  ProvisioningForm ok_raw;
  ProvisioningForm ok_enc;
  // Printable '~' (0x7e): raw and %7e both accepted and equal.
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok), P(parseBody("ssid=a~b", &ok_raw)));
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("ssid=a%7eb", &ok_enc)));
  TEST_ASSERT_EQUAL_STRING("a~b", ok_raw.ssid.value);
  TEST_ASSERT_EQUAL_STRING("a~b", ok_enc.ssid.value);
  // High byte 0xE9: raw and %E9 both rejected.
  ProvisioningForm f;
  const char raw[] = {'s', 's', 'i', 'd', '=', static_cast<char>(0xE9)};
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding),
                        P(core::urlFormParse(raw, sizeof(raw), 12, &f)));
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::BadEncoding),
                        P(parseBody("ssid=%E9", &f)));
}

void test_duplicate_known_field_rejected() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::DuplicateField),
                        P(parseBody("ssid=a&ssid=b", &f)));
}

void test_unknown_fields_ignored_including_duplicates() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("foo=1&foo=2&bar=x&csrf=tok", &f)));
  TEST_ASSERT_TRUE(f.csrf.present);
  TEST_ASSERT_EQUAL_STRING("tok", f.csrf.value);
  TEST_ASSERT_FALSE(f.ssid.present);
}

void test_too_many_fields() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::TooManyFields),
                        P(parseBody("a=1&b=2&c=3&d=4", &f, /*max_pairs=*/3)));
  // Exactly at the limit is accepted.
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("a=1&b=2&c=3", &f, /*max_pairs=*/3)));
}

void test_field_name_too_long() {
  ProvisioningForm f;
  // A 33-char key (> kFormFieldNameCap of 32), then "=1".
  char req[64];
  int n = 0;
  for (; n < 33; ++n) {
    req[n] = 'k';
  }
  req[n++] = '=';
  req[n++] = '1';
  req[n] = '\0';
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::FieldNameTooLong), P(parseBody(req, &f)));
}

void test_field_value_too_long() {
  ProvisioningForm f;
  char req[160];
  char val[120];
  std::memset(val, 'v', 97);  // 97 > kFormFieldValueCap (96)
  val[97] = '\0';
  std::snprintf(req, sizeof(req), "ssid=%s", val);
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::FieldValueTooLong),
                        P(parseBody(req, &f)));
}

void test_malformed_pairs() {
  ProvisioningForm f;
  const char* bads[] = {"ssid", "=value", "a=1&&b=2", "&a=1"};
  for (const char* b : bads) {
    TEST_ASSERT_EQUAL_INT(P(FormParseResult::MalformedPair), P(parseBody(b, &f)));
  }
}

void test_empty_body_and_trailing_amp() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok), P(parseBody("", &f)));
  TEST_ASSERT_FALSE(f.csrf.present);
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok), P(parseBody("csrf=x&", &f)));
  TEST_ASSERT_TRUE(f.csrf.present);
}

void test_checkbox_presence() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(P(FormParseResult::Ok),
                        P(parseBody("use_miles=1&csrf=t", &f)));
  TEST_ASSERT_TRUE(core::formCheckboxOn(f.use_miles));
  TEST_ASSERT_FALSE(core::formCheckboxOn(f.show_runways));
}

// --- semantic validation ----------------------------------------------------

void test_valid_provisioning_ok() {
  ProvisioningForm f;
  makeValid(&f);
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok),
                        V(core::validateProvisioning(f, 32)));
}

void test_csrf_missing_and_wrong_length() {
  ProvisioningForm f;
  TEST_ASSERT_EQUAL_INT(
      P(FormParseResult::Ok),
      P(parseBody("ssid=Home&psk=password1&lat=52.37&lon=4.90", &f)));
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::CsrfMissing),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::CsrfInvalid),
                        V(core::validateProvisioning(f, 31)));  // expects 31
}

void test_ssid_rules() {
  ProvisioningForm f;
  makeValid(&f);
  f.ssid.present = false;
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::SsidMissing),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  f.ssid.len = 0;  // empty
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::SsidInvalid),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  f.ssid.len = 33;  // over 32
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::SsidInvalid),
                        V(core::validateProvisioning(f, 32)));
}

void test_psk_length_boundaries() {
  ProvisioningForm f;
  makeValid(&f);
  f.psk.len = 0;  // empty = open network, allowed
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok),
                        V(core::validateProvisioning(f, 32)));
  f.psk.len = 7;  // too short
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::PskInvalid),
                        V(core::validateProvisioning(f, 32)));
  f.psk.len = 8;  // minimum WPA2
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok),
                        V(core::validateProvisioning(f, 32)));
  f.psk.len = 63;  // maximum
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok),
                        V(core::validateProvisioning(f, 32)));
  f.psk.len = 64;  // too long
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::PskInvalid),
                        V(core::validateProvisioning(f, 32)));
}

void test_missing_psk_and_coords() {
  ProvisioningForm f;
  makeValid(&f);
  f.psk.present = false;
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::PskMissing),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  f.lat.present = false;
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::LatMissing),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  f.lon.present = false;
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::LonMissing),
                        V(core::validateProvisioning(f, 32)));
  makeValid(&f);
  f.lat.len = 0;  // empty coordinate string
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::LatInvalid),
                        V(core::validateProvisioning(f, 32)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_basic_known_fields);
  RUN_TEST(test_percent_and_plus_decoding);
  RUN_TEST(test_malformed_percent_rejected);
  RUN_TEST(test_decoded_control_or_nul_rejected);
  RUN_TEST(test_raw_control_byte_rejected);
  RUN_TEST(test_encoded_high_bytes_rejected);
  RUN_TEST(test_raw_high_byte_rejected);
  RUN_TEST(test_raw_and_encoded_equivalence);
  RUN_TEST(test_duplicate_known_field_rejected);
  RUN_TEST(test_unknown_fields_ignored_including_duplicates);
  RUN_TEST(test_too_many_fields);
  RUN_TEST(test_field_name_too_long);
  RUN_TEST(test_field_value_too_long);
  RUN_TEST(test_malformed_pairs);
  RUN_TEST(test_empty_body_and_trailing_amp);
  RUN_TEST(test_checkbox_presence);
  RUN_TEST(test_valid_provisioning_ok);
  RUN_TEST(test_csrf_missing_and_wrong_length);
  RUN_TEST(test_ssid_rules);
  RUN_TEST(test_psk_length_boundaries);
  RUN_TEST(test_missing_psk_and_coords);
  return UNITY_END();
}
