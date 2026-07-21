#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/portal_auth.h"
#include "core/portal_secrets.h"
#include "core/url_form.h"

using core::AuthorizedProvisioning;
using core::ProvisioningForm;
using core::ProvisioningValidity;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int V(ProvisioningValidity r) { return static_cast<int>(r); }

// A 32-hex-char CSRF token (the issued length: core::kPortalCsrfTokenLen).
const char* kSessionToken = "0123456789abcdef0123456789abcdef";
const char* kOtherToken = "fedcba9876543210fedcba9876543210";

// Build a provisioning form carrying `csrf` and otherwise-valid fields.
void makeForm(const char* csrf, ProvisioningForm* out) {
  char body[160];
  std::snprintf(body, sizeof(body),
                "csrf=%s&ssid=HomeNet&psk=password1&lat=52.37&lon=4.90", csrf);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::FormParseResult::Ok),
      static_cast<int>(core::urlFormParse(
          body, static_cast<uint16_t>(std::strlen(body)), 12, out)));
}

AuthorizedProvisioning auth(const ProvisioningForm& f, const char* token,
                            uint32_t session_id = 7) {
  return core::authenticateProvisioning(
      f, token, static_cast<uint16_t>(std::strlen(kSessionToken)), session_id);
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- capability: a default artifact carries no authority (fix 6) ------------

void test_default_artifact_is_inert() {
  AuthorizedProvisioning a;  // no public way to set authorized_ from outside
  TEST_ASSERT_FALSE(a.authorized());
  TEST_ASSERT_FALSE(a.csrf_match());
  TEST_ASSERT_EQUAL_UINT32(0, a.session_id());
}

// --- happy path -------------------------------------------------------------

void test_correct_token_authorizes_and_binds_session() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  AuthorizedProvisioning r = auth(f, kSessionToken, /*session_id=*/42);
  TEST_ASSERT_TRUE(r.authorized());
  TEST_ASSERT_TRUE(r.csrf_match());
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok), V(r.validity()));
  TEST_ASSERT_EQUAL_UINT32(42, r.session_id());
}

// --- wrong / cross-session tokens -------------------------------------------

void test_wrong_token_same_length_is_unauthorized() {
  ProvisioningForm f;
  makeForm(kOtherToken, &f);  // form carries the attacker's token
  AuthorizedProvisioning r = auth(f, kSessionToken);  // session issued another
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_FALSE(r.csrf_match());
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::Ok), V(r.validity()));  // len ok
}

void test_previous_session_token_rejected() {
  // A form minted for a previous session (kOtherToken) replayed against the
  // current session (kSessionToken) fails the token comparison.
  ProvisioningForm f;
  makeForm(kOtherToken, &f);
  AuthorizedProvisioning r = auth(f, kSessionToken, /*session_id=*/2);
  TEST_ASSERT_FALSE(r.authorized());
}

void test_last_byte_difference_detected() {
  char almost[33];
  std::strcpy(almost, kSessionToken);
  almost[31] = (almost[31] == 'f') ? 'e' : 'f';  // flip only the last hex char
  ProvisioningForm f;
  makeForm(almost, &f);
  AuthorizedProvisioning r = auth(f, kSessionToken);
  TEST_ASSERT_FALSE(r.authorized());  // full-length compare catches the last byte
}

// --- validation failures short-circuit before authorization ------------------

void test_missing_field_is_unauthorized() {
  ProvisioningForm f;
  // Valid csrf but no ssid: semantic validation fails, so it cannot authorize.
  char body[80];
  std::snprintf(body, sizeof(body), "csrf=%s&psk=password1&lat=1.0&lon=2.0",
                kSessionToken);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::FormParseResult::Ok),
      static_cast<int>(core::urlFormParse(
          body, static_cast<uint16_t>(std::strlen(body)), 12, &f)));
  AuthorizedProvisioning r = auth(f, kSessionToken);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_FALSE(r.csrf_match());
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::SsidMissing), V(r.validity()));
}

void test_wrong_csrf_length_in_form_is_unauthorized() {
  ProvisioningForm f;
  makeForm("tooshort", &f);  // 8-char csrf in the body, not 32
  AuthorizedProvisioning r = auth(f, kSessionToken);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::CsrfInvalid), V(r.validity()));
}

void test_missing_csrf_is_unauthorized() {
  ProvisioningForm f;
  char body[80];
  std::snprintf(body, sizeof(body), "ssid=HomeNet&psk=password1&lat=1.0&lon=2.0");
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::FormParseResult::Ok),
      static_cast<int>(core::urlFormParse(
          body, static_cast<uint16_t>(std::strlen(body)), 12, &f)));
  AuthorizedProvisioning r = auth(f, kSessionToken);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_EQUAL_INT(V(ProvisioningValidity::CsrfMissing), V(r.validity()));
}

// --- CSRF API floor (fix 7) -------------------------------------------------

void test_null_session_token_is_unauthorized() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  AuthorizedProvisioning r = core::authenticateProvisioning(
      f, nullptr, static_cast<uint16_t>(std::strlen(kSessionToken)), 1);
  TEST_ASSERT_FALSE(r.authorized());
}

void test_zero_session_id_is_unauthorized() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  AuthorizedProvisioning r = core::authenticateProvisioning(
      f, kSessionToken, static_cast<uint16_t>(std::strlen(kSessionToken)),
      /*session_id=*/0);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_EQUAL_UINT32(0, r.session_id());
}

void test_zero_token_len_is_unauthorized() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  // An empty issued token can never authenticate, even with a matching form.
  AuthorizedProvisioning r =
      core::authenticateProvisioning(f, kSessionToken, /*token_len=*/0, 9);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_FALSE(r.csrf_match());
}

void test_wrong_token_len_is_unauthorized() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  // A token length other than kPortalCsrfTokenLen (32) is rejected by the floor
  // before any comparison -- here 16, a common truncation.
  AuthorizedProvisioning r =
      core::authenticateProvisioning(f, kSessionToken, /*token_len=*/16, 9);
  TEST_ASSERT_FALSE(r.authorized());
  TEST_ASSERT_FALSE(r.csrf_match());
  // The far side of the boundary (33) is likewise rejected.
  AuthorizedProvisioning r2 = core::authenticateProvisioning(
      f, kSessionToken, static_cast<uint16_t>(core::kPortalCsrfTokenLen + 1), 9);
  TEST_ASSERT_FALSE(r2.authorized());
}

void test_exact_token_len_boundary_authorizes() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  AuthorizedProvisioning r = core::authenticateProvisioning(
      f, kSessionToken, core::kPortalCsrfTokenLen, 9);
  TEST_ASSERT_TRUE(r.authorized());
}

// --- replay (deterministic, pure) -------------------------------------------

void test_authenticate_is_deterministic_for_replay() {
  ProvisioningForm f;
  makeForm(kSessionToken, &f);
  AuthorizedProvisioning a = auth(f, kSessionToken, 5);
  AuthorizedProvisioning b = auth(f, kSessionToken, 5);
  TEST_ASSERT_TRUE(a.authorized());
  TEST_ASSERT_TRUE(b.authorized());
  TEST_ASSERT_EQUAL_UINT32(a.session_id(), b.session_id());
  // Single-use enforcement is trial-bound in the state machine, not here.
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_default_artifact_is_inert);
  RUN_TEST(test_correct_token_authorizes_and_binds_session);
  RUN_TEST(test_wrong_token_same_length_is_unauthorized);
  RUN_TEST(test_previous_session_token_rejected);
  RUN_TEST(test_last_byte_difference_detected);
  RUN_TEST(test_missing_field_is_unauthorized);
  RUN_TEST(test_wrong_csrf_length_in_form_is_unauthorized);
  RUN_TEST(test_missing_csrf_is_unauthorized);
  RUN_TEST(test_null_session_token_is_unauthorized);
  RUN_TEST(test_zero_session_id_is_unauthorized);
  RUN_TEST(test_zero_token_len_is_unauthorized);
  RUN_TEST(test_wrong_token_len_is_unauthorized);
  RUN_TEST(test_exact_token_len_boundary_authorizes);
  RUN_TEST(test_authenticate_is_deterministic_for_replay);
  return UNITY_END();
}
