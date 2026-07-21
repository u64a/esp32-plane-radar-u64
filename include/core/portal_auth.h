#pragma once

#include <cstdint>

#include "core/url_form.h"

// Authenticated provisioning gate (pure, Arduino-free).
//
// This is the ONLY way a decoded provisioning form becomes eligible to drive the
// portal state machine's candidate submission. It deliberately fuses four checks
// that must all hold before a Wi-Fi candidate may be trialed, so that a mere
// length-valid form (validateProvisioning() == Ok) can never by itself authorize
// a submission:
//
//   0. CSRF API floor -- session_id != 0, a non-null issued token, and a token
//      length of exactly kPortalCsrfTokenLen. A malformed call fails closed
//      before any comparison.
//   1. Semantic field validation (core::validateProvisioning) -- required fields
//      present and within their bounds, and the submitted CSRF token has exactly
//      the issued length. Length is not secret and is checked first.
//   2. Constant-time, full-length comparison of the submitted CSRF token against
//      the token issued for THIS session (core::csrfTokenEqual). Timing does not
//      leak the match prefix.
//   3. Session binding -- the result carries the session_id it was authenticated
//      for, so the state machine can reject an artifact minted for another session
//      (a cross-session replay) even if the token somehow compared equal.
//
// The returned artifact is a capability: only authenticateProvisioning() can mint
// an authorized, session-bound value, so no caller (or test) can fabricate one by
// setting a public field. It is never logged; the CSRF bytes are not copied in.

namespace core {

// The typed outcome of authenticating a provisioning form for a specific
// session. This is a capability, not a plain aggregate: an *authorized* artifact
// (one whose authorized() is true and whose session_id() binds it to a session)
// can ONLY be minted by authenticateProvisioning(). A default-constructed value
// is inert -- authorized() is false and session_id() is 0 -- and no public
// member lets a caller flip it to authorized. This closes the forgery hole where
// a caller/test could set authorized=true without ever running the form + token
// authentication path.
//
// The read-only accessors expose truthful diagnostics (never the secret itself):
//   * authorized() -- true iff validation, the constant-time token comparison,
//     and the CSRF API floor all succeeded;
//   * csrf_match()  -- whether the constant-time token comparison matched;
//   * validity()    -- the semantic validation outcome;
//   * session_id()  -- the session the artifact was authenticated against (0 for
//     an inert/unauthorized default).
class AuthorizedProvisioning {
 public:
  // A default artifact carries no authority: unauthorized, unbound (session 0).
  AuthorizedProvisioning() = default;

  bool authorized() const { return authorized_; }
  bool csrf_match() const { return csrf_match_; }
  ProvisioningValidity validity() const { return validity_; }
  uint32_t session_id() const { return session_id_; }

 private:
  // Only the authenticator may construct an authorized/session-bound artifact.
  friend AuthorizedProvisioning authenticateProvisioning(
      const ProvisioningForm& form, const char* session_token,
      uint16_t token_len, uint32_t session_id);

  bool authorized_ = false;
  bool csrf_match_ = false;
  ProvisioningValidity validity_ = ProvisioningValidity::CsrfMissing;
  uint32_t session_id_ = 0;
};

// Authenticate `form` for the active session identified by `session_id` (which
// must be nonzero), whose issued CSRF token is `session_token` (holding
// `token_len` bytes). Enforces the CSRF API floor first -- session_id != 0, a
// non-null session_token, and token_len == kPortalCsrfTokenLen -- then performs
// semantic validation (with `token_len` as the expected CSRF length) and finally
// a constant-time full-length token comparison. On any failure the result is
// unauthorized (authorized() == false) and carries the specific reason. The
// form's raw CSRF bytes are never copied into the result.
AuthorizedProvisioning authenticateProvisioning(const ProvisioningForm& form,
                                                const char* session_token,
                                                uint16_t token_len,
                                                uint32_t session_id);

}  // namespace core
