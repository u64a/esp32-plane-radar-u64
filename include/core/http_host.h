#pragma once

#include <cstdint>

// Portal Host-authority policy for the captive setup server (pure, Arduino-free).
//
// The SoftAP answers a wildcard captive DNS, so an attacker-controlled hostname
// can be made to resolve to the AP (DNS rebinding). A browser sends the URL's
// authority verbatim in the Host header, so the ONLY request whose Host is a
// canonical portal authority is one the browser already treats as same-origin
// with the real portal at http://192.168.4.1/. Gating the sensitive form and its
// POST on this exact-match classification is therefore the rebinding defense: an
// attacker page rebound to the AP still carries its OWN Host (evil.example), and
// so can never retrieve the CSRF-bearing form nor submit /save.
//
// This header owns the single source of truth for that policy so the parser (who
// alone sees the Host bytes) and the router (who alone owns the route table) agree
// without either storing or reflecting the untrusted value. Only a boolean
// classification is ever kept; the raw authority is compared here and discarded.

namespace core {

// The canonical portal authorities, matched EXACTLY (ASCII, byte-for-byte). A
// browser omits the default port for http, but a client may include ":80", so
// both spellings of the same authority are canonical. These are numeric+colon
// only, so there is no case or IDN ambiguity to fold. They must mirror
// config::kPortalIp (the SoftAP address); config.h is Arduino-coupled and cannot
// be included here, so the value is duplicated intentionally as the pure-core
// source of truth.
inline constexpr char kPortalAuthority[] = "192.168.4.1";
inline constexpr char kPortalAuthorityWithPort[] = "192.168.4.1:80";

// Fixed redirect target for a noncanonical Host on an application route. The
// adapter emits `302 Location: <this>` with an empty body and no portal secret.
// It is a constant of the portal policy -- NEVER derived from the request Host --
// so an attacker Host can never appear in a Location header.
inline constexpr char kPortalRedirectLocation[] = "http://192.168.4.1/";

// True iff [host, len) is exactly one of the canonical portal authorities above.
// The value is interpreted and discarded; nothing is stored or reflected. A null
// pointer or empty span (a missing/empty Host) is not canonical.
inline bool httpHostIsCanonicalPortal(const char* host, uint16_t len) {
  if (host == nullptr || len == 0) {
    return false;
  }
  const char* const candidates[] = {kPortalAuthority, kPortalAuthorityWithPort};
  for (const char* lit : candidates) {
    uint16_t i = 0;
    bool equal = true;
    for (; i < len; ++i) {
      if (lit[i] == '\0' || host[i] != lit[i]) {
        equal = false;  // literal ended early, or a byte differs
        break;
      }
    }
    if (equal && lit[i] == '\0') {
      return true;  // spans matched and both ended together
    }
  }
  return false;
}

}  // namespace core
