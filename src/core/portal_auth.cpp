#include "core/portal_auth.h"

#include "core/portal_secrets.h"

namespace core {

AuthorizedProvisioning authenticateProvisioning(const ProvisioningForm& form,
                                                const char* session_token,
                                                uint16_t token_len,
                                                uint32_t session_id) {
  AuthorizedProvisioning out;
  out.authorized_ = false;
  out.csrf_match_ = false;
  out.validity_ = ProvisioningValidity::CsrfInvalid;
  out.session_id_ = session_id;

  // CSRF API floor (fail closed before any comparison): a well-formed call must
  // bind to a real session (session_id != 0), hold a non-null issued token, and
  // present exactly kPortalCsrfTokenLen (32) encoded characters. token_len == 0
  // and any other length are rejected here, so a caller cannot slip a truncated
  // or absent token past the constant-time compare.
  if (session_id == 0U || session_token == nullptr || token_len == 0U ||
      token_len != kPortalCsrfTokenLen) {
    return out;
  }

  // Semantic validation. This also proves form.csrf is present and has exactly
  // token_len bytes, which the constant-time compare below relies on.
  out.validity_ = validateProvisioning(form, token_len);
  if (out.validity_ != ProvisioningValidity::Ok) {
    return out;
  }

  // Full-length, constant-time comparison against the session's issued token.
  out.csrf_match_ = csrfTokenEqual(form.csrf.value, session_token, token_len);
  out.authorized_ = out.csrf_match_;
  return out;
}

}  // namespace core
