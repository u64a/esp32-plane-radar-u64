#pragma once

#include <cstdint>

#include "core/http_host.h"
#include "core/http_request.h"

// Closed-set request router for the captive setup portal.
//
// The portal exposes a tiny, fixed surface. Everything is matched against an
// explicit allow-list (exact, case-sensitive path match) plus a small deny-list
// for known-dangerous WiFiManager/OTA endpoints. There is no wildcard, prefix,
// or filesystem mapping, so nothing outside this table can ever be reached.
//
// The path handed in must already be the parser's validated, non-normalized path
// (leading '/', no traversal, no backslash, no percent-encoding, no repeated
// slash). Combined with exact matching, that makes route aliasing impossible.
//
// Host gating (DNS-rebinding defense). The AP runs a wildcard captive DNS, so an
// attacker-controlled hostname can be rebound to the AP. The two APPLICATION
// routes are therefore gated on an exact-match Host classification (see
// core::httpHostIsCanonicalPortal), independent of HTTP version:
//   * GET /      -> the CSRF-bearing form ONLY for a canonical portal Host;
//                   any other (or absent) Host resolves to HttpRoute::RedirectToPortal,
//                   a fixed token- and body-free 302 to core::kPortalRedirectLocation.
//   * POST /save -> accepted ONLY for a canonical portal Host; any other (or
//                   absent) Host is Denied and never reaches the save handler.
// A Host-less HTTP/1.0 request is thus classed noncanonical and can neither
// retrieve nor submit the sensitive form. The captive-probe routes are NOT
// host-gated -- external OS probes deliberately send their own hostnames and
// receive fixed, token-free responses -- and unknown paths stay 404. The router
// never emits or reflects the request Host: the redirect target is a constant.
//
// The Host classification is a REQUIRED input on every resolve entry point, so a
// caller cannot accidentally route without it. Prefer the HttpRequest overload.

namespace core {

// Every route the portal recognizes. Kept explicit so the adapter dispatches on
// a value rather than re-comparing strings.
enum class HttpRoute : uint8_t {
  None = 0,
  Root,                   // GET / (canonical Host): the setup form
  Save,                   // POST /save (canonical Host): candidate submission
  RedirectToPortal,       // GET / (noncanonical/absent Host): fixed 302 to the
                          // portal authority (no token, no body)
  CaptiveGenerate204,     // GET /generate_204
  CaptiveGen204,          // GET /gen_204
  CaptiveHotspotDetect,   // GET /hotspot-detect.html
  CaptiveNcsi,            // GET /ncsi.txt
  CaptiveConnectTest,     // GET /connecttest.txt
  CaptiveCanonical,       // GET /canonical.html
  CaptiveSuccessTxt,      // GET /success.txt
  CaptiveLibrarySuccess,  // GET /library/test/success.html
};

// Outcome of resolving a (method, path, host) triple.
enum class RouteResult : uint8_t {
  Matched = 0,       // a route matched; *out_route is set
  NotFound,          // path is not in the allow-list (404)
  MethodNotAllowed,  // path is known but the method is wrong (405)
  Denied,            // path is a forbidden danger route, or a noncanonical-Host
                     // POST /save (403/404)
};

// Host classification the router requires to gate the application routes. Derived
// from the parser's exact-match verdict; there is no default, so a caller must
// state it explicitly and cannot silently skip the rebinding check.
enum class PortalHostClass : uint8_t {
  NonCanonical = 0,  // Host absent, or any authority other than the portal's
  CanonicalPortal,   // Host is exactly 192.168.4.1 or 192.168.4.1:80
};

// Resolve a parsed request, host-gating the application routes on its classified
// Host (req.host_is_portal). This is the preferred entry point: the request
// carries method, validated path, and Host verdict together, so host
// classification can never be forgotten. On RouteResult::Matched, *out_route
// receives the route (Root/Save/RedirectToPortal/probe); otherwise HttpRoute::None.
RouteResult httpRouteResolve(const HttpRequest& req, HttpRoute* out_route);

// Lower-level resolve with an EXPLICIT Host classification (required). Danger
// routes are checked first, case-insensitively, so /update, /Update, and /UPDATE
// are all Denied. Allowed routes are matched exactly and case-sensitively (so
// /Save is simply NotFound, never /save). A known path reached with the wrong
// method yields MethodNotAllowed. The application routes (/ and /save) are then
// gated on `host` as documented above; probe routes ignore it.
RouteResult httpRouteResolve(HttpMethod method, const char* path,
                             uint16_t path_len, PortalHostClass host,
                             HttpRoute* out_route);

}  // namespace core
