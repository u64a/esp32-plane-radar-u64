#include "core/http_router.h"

#include <cstddef>

namespace core {

namespace {

uint8_t toLowerAscii(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c - 'A' + 'a') : c;
}

// Length of a NUL-terminated ASCII literal.
uint16_t litLen(const char* s) {
  uint16_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }
  return n;
}

// Exact, case-sensitive equality of a (data,len) span and a literal.
bool exactEqual(const char* data, uint16_t len, const char* lit) {
  if (litLen(lit) != len) {
    return false;
  }
  for (uint16_t i = 0; i < len; ++i) {
    if (data[i] != lit[i]) {
      return false;
    }
  }
  return true;
}

// Case-insensitive equality of a (data,len) span and a literal.
bool ciEqual(const char* data, uint16_t len, const char* lit) {
  if (litLen(lit) != len) {
    return false;
  }
  for (uint16_t i = 0; i < len; ++i) {
    if (toLowerAscii(static_cast<uint8_t>(data[i])) !=
        toLowerAscii(static_cast<uint8_t>(lit[i]))) {
      return false;
    }
  }
  return true;
}

struct RouteEntry {
  HttpMethod method;
  const char* path;
  HttpRoute route;
};

// The complete allow-list. Order is irrelevant (paths are unique).
constexpr RouteEntry kRoutes[] = {
    {HttpMethod::Get, "/", HttpRoute::Root},
    {HttpMethod::Post, "/save", HttpRoute::Save},
    {HttpMethod::Get, "/generate_204", HttpRoute::CaptiveGenerate204},
    {HttpMethod::Get, "/gen_204", HttpRoute::CaptiveGen204},
    {HttpMethod::Get, "/hotspot-detect.html", HttpRoute::CaptiveHotspotDetect},
    {HttpMethod::Get, "/ncsi.txt", HttpRoute::CaptiveNcsi},
    {HttpMethod::Get, "/connecttest.txt", HttpRoute::CaptiveConnectTest},
    {HttpMethod::Get, "/redirect", HttpRoute::RedirectToPortal},
    {HttpMethod::Get, "/canonical.html", HttpRoute::CaptiveCanonical},
    {HttpMethod::Get, "/success.txt", HttpRoute::CaptiveSuccessTxt},
    {HttpMethod::Get, "/library/test/success.html",
     HttpRoute::CaptiveLibrarySuccess},
};

// Explicitly forbidden endpoints: WiFiManager config/OTA/reboot/erase surfaces
// that must never do anything on this device. Matched case-insensitively so
// spelling variants are also denied. (Traversal spellings never reach here --
// the parser rejects them outright.)
constexpr const char* kDenyPaths[] = {
    "/update", "/u",       "/erase", "/reset",     "/reboot",
    "/restart", "/info",   "/fwlink", "/setwifisave",
};

}  // namespace

RouteResult httpRouteResolve(HttpMethod method, const char* path,
                             uint16_t path_len, PortalHostClass host,
                             HttpRoute* out_route) {
  if (out_route != nullptr) {
    *out_route = HttpRoute::None;
  }
  if (path == nullptr || path_len == 0) {
    return RouteResult::NotFound;
  }

  for (const char* deny : kDenyPaths) {
    if (ciEqual(path, path_len, deny)) {
      return RouteResult::Denied;
    }
  }

  const bool host_portal = host == PortalHostClass::CanonicalPortal;

  bool path_known = false;
  for (const RouteEntry& e : kRoutes) {
    if (exactEqual(path, path_len, e.path)) {
      path_known = true;
      if (e.method == method) {
        // Method matches. Apply per-route Host gating: the two application routes
        // are gated on the canonical portal authority; probe routes are not.
        switch (e.route) {
          case HttpRoute::Root:
            // GET /: the CSRF-bearing form ONLY for a canonical portal Host; any
            // other (or absent) Host gets a fixed token-free redirect instead, so
            // a rebound attacker origin can never read the form as same-origin.
            if (out_route != nullptr) {
              *out_route = host_portal ? HttpRoute::Root
                                       : HttpRoute::RedirectToPortal;
            }
            return RouteResult::Matched;
          case HttpRoute::Save:
            // POST /save: accepted ONLY for a canonical portal Host. A
            // noncanonical Host is Denied and never reaches the save handler.
            if (!host_portal) {
              return RouteResult::Denied;
            }
            if (out_route != nullptr) {
              *out_route = HttpRoute::Save;
            }
            return RouteResult::Matched;
          default:
            // Captive probes: fixed, token-free responses, host-agnostic.
            if (out_route != nullptr) {
              *out_route = e.route;
            }
            return RouteResult::Matched;
        }
      }
    }
  }
  return path_known ? RouteResult::MethodNotAllowed : RouteResult::NotFound;
}

RouteResult httpRouteResolve(const HttpRequest& req, HttpRoute* out_route) {
  return httpRouteResolve(req.method, req.path, req.path_len,
                          req.host_is_portal ? PortalHostClass::CanonicalPortal
                                             : PortalHostClass::NonCanonical,
                          out_route);
}

}  // namespace core
