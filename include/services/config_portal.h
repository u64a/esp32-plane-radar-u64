#pragma once

// Temporary captive setup portal transport (ESP-only). Owns an Arduino
// WiFiServer (bound to the EXACT SoftAP IP with a one-client backlog), the
// captive DNSServer, at most one accepted WiFiClient, the incremental HTTP parser
// workspace, and a fixed response workspace. It exists ONLY while the SoftAP is
// active: services::wifi_setup starts it after the SoftAP comes up and stops it
// before the SoftAP is torn down. It never persists, never allocates per request,
// and never owns Wi-Fi credentials or secrets beyond the pointers handed to it.
//
// Bytes are pumped with non-blocking lwip_recv / lwip_send on the accepted
// WiFiClient::fd() (never WiFiClient::read()/write()), so the request PSK never
// lands in WiFiClient's internal 1436-byte RxBuffer, and each raw read chunk is
// wiped immediately after it is fed to the parser.
//
// It feeds core::HttpRequestParser one buffer at a time, resolves every request
// through core::httpRouteResolve (never by raw path), and answers only the fixed
// closed route set: Root, Save, RedirectToPortal, the nine captive OS routes, and
// truthful 4xx/5xx for everything else. Responses are bounded and allocation-free
// (one fixed buffer, sent through non-blocking lwip_send with a bounded deadline
// that also aborts the moment the original portal session deadline expires).

#include <cstddef>
#include <cstdint>

#include "core/http_request.h"

namespace services::portal {

// The owner's decision for an authenticated POST /save, mapped to a fixed HTTP
// response. The portal never authenticates or stages itself; it delegates to the
// handler and only renders the truthful result.
enum class SaveOutcome : uint8_t {
  Accepted,       // staged + submitted: serve the "testing" page; teardown follows
  BadRequest,     // 400: malformed / undecodable form body
  Forbidden,      // 403: CSRF / authentication failure
  Unprocessable,  // 422: semantic validation failure (ssid/psk/coordinates)
};

using SaveHandler = SaveOutcome (*)(const core::HttpRequest& request, void* ctx);
using PumpFn = void (*)();
using ExpiredFn = bool (*)();

// Trusted local content used to render the root form + answer DNS. Fixed values
// are copied; csrf_token must remain valid until stop().
struct PortalContent {
  uint8_t ap_ip[4];        // SoftAP address for DNS answers + redirect base
  const char* csrf_token;  // 32-char per-session token (hidden form field)
  double lat;              // trusted local defaults to prefill the form
  double lon;
  bool use_miles;
  bool show_runways;
  SaveHandler on_save;     // authenticates + stages a submission
  void* on_save_ctx;
};

// Begin serving (SoftAP must already be up at content.ap_ip). Binds the captive
// DNS and an Arduino WiFiServer to the EXACT SoftAP IP (one-client backlog).
// Returns false unless BOTH the DNS server is bound AND the WiFiServer reports it
// is actually listening (operator bool()); on any partial failure it tears down
// what it started and reports false so the caller never believes a listener exists.
bool start(const PortalContent& content);

// Pump one iteration: one captive DNS query, then advance the single client
// (accept, incremental parse, and on Complete/Error/timeout write one fixed
// response and close). Non-blocking apart from the bounded response write. If the
// injected expiry predicate reports the session deadline has passed, no client is
// accepted or served and any in-flight write aborts, so HTTP/DNS/AP are not held
// open for a response budget past the deadline.
void pump();

// Tear down client + HTTP (WiFiServer::end()) + DNS. Idempotent. (SoftAP teardown
// is the owner's job.)
void stop();

// True while the listener is bound.
bool active();

// Inject the inter-write pump (button + DNS) invoked between non-blocking write
// attempts so a configure/erase gesture stays responsive. Optional.
void setPumpFn(PumpFn fn);

// Inject a predicate that returns true once the ORIGINAL portal session deadline
// has expired (candidate retries never extend it). When set, pump() stops
// accepting/serving and any in-flight response write aborts immediately, so the
// HTTP/DNS/AP surface is never held open for the write budget past the deadline.
// Optional; when unset the portal relies only on its own bounded write deadline.
void setExpiredFn(ExpiredFn fn);

}  // namespace services::portal
