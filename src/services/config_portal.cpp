#include "services/config_portal.h"

#include <DNSServer.h>
#include <WiFi.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <lwip/sockets.h>

#include "config.h"
#include "core/http_host.h"
#include "core/http_router.h"
#include "core/portal_secrets.h"  // core::secureZero (workspace wipes)
#include "core/time_math.h"

namespace services::portal {

namespace {

// Captive HTTP listener: an Arduino WiFiServer bound to the EXACT SoftAP IP
// (192.168.4.1) with a one-client backlog, so it listens ONLY on the SoftAP
// interface -- never INADDR_ANY. The accepted client's bytes are pumped with
// non-blocking lwip_recv / lwip_send on WiFiClient::fd() (never the buffered
// WiFiClient::read()/write()), so the request PSK never lands in WiFiClient's
// internal 1436-byte RxBuffer.
WiFiServer s_server(IPAddress(config::kPortalIpOctets[0], config::kPortalIpOctets[1],
                              config::kPortalIpOctets[2], config::kPortalIpOctets[3]),
                    config::kPortalHttpPort, /*max_clients=*/1);
DNSServer s_dns;
WiFiClient s_client;
core::HttpRequestParser s_parser;

bool s_active = false;
bool s_client_active = false;
uint32_t s_conn_started_ms = 0;
uint32_t s_conn_last_byte_ms = 0;

PortalContent s_content = {};
PumpFn s_pump_fn = nullptr;
ExpiredFn s_expired_fn = nullptr;

// One fixed response-assembly buffer plus a body scratch. Neither grows; the root
// form is the largest producer and fits comfortably.
constexpr size_t kBodyCap = 1600;
constexpr size_t kOutCap = 2048;
char s_body[kBodyCap];
char s_out[kOutCap];

// True once the injected expiry predicate reports the session deadline has passed.
bool sessionExpired() { return s_expired_fn != nullptr && s_expired_fn(); }

// Securely wipe the per-request workspaces. The parsed request body can contain
// the user's home Wi-Fi PSK (raw POST), and the response/body buffers can hold
// the CSRF token, so both are zeroed on every close and on stop().
void wipeWorkspaces() {
  core::secureZero(&s_parser, sizeof(s_parser));
  core::secureZero(s_body, sizeof(s_body));
  core::secureZero(s_out, sizeof(s_out));
}

void pumpBetween() {
  s_dns.processNextRequest();
  if (s_pump_fn != nullptr) {
    s_pump_fn();
  }
}

// Send the whole buffer through non-blocking lwip_send with a cumulative bounded
// deadline, pumping DNS/button between attempts so a WiFiClient::write ~10 s retry
// stall can never happen. The write also aborts the moment the original session
// deadline expires, so no response budget can hold the AP/HTTP open past five
// minutes. One response, then the caller closes.
bool writeAll(const char* data, size_t len) {
  const int fd = s_client.fd();
  if (fd < 0) {
    return false;
  }
  size_t sent = 0;
  const uint32_t started = millis();
  while (sent < len) {
    if (sessionExpired()) {
      return false;  // session deadline passed: abort the response immediately
    }
    if (core::elapsedAtLeast(millis(), started,
                             config::kPortalHttpWriteDeadlineMs)) {
      return false;  // bounded write deadline exceeded
    }
    const int w = lwip_send(fd, data + sent, len - sent, MSG_DONTWAIT);
    if (w > 0) {
      sent += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
      return false;  // real socket error
    }
    pumpBetween();  // back-pressure or would-block: yield and retry
  }
  return true;
}

// A fixed, self-contained 500 sent verbatim (never composed) when a response
// would otherwise be truncated. Content-Length matches the 26-byte body exactly.
constexpr char kFixed500Response[] =
    "HTTP/1.1 500 Internal Server Error\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 26\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "\r\n"
    "500 Internal Server Error\n";

void sendFixed500() { writeAll(kFixed500Response, strlen(kFixed500Response)); }

// Compose status line + fixed headers + body into s_out and send once. If the
// header or the header+body would not fit the fixed buffer, a fixed 500 is sent
// instead of a truncated response (never a short/mismatched Content-Length).
void sendResponse(const char* status, const char* content_type,
                  const char* extra_headers, const char* body,
                  size_t body_len) {
  int header_len = std::snprintf(
      s_out, sizeof(s_out),
      "HTTP/1.1 %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %u\r\n"
      "Connection: close\r\n"
      "Cache-Control: no-store\r\n"
      "%s"
      "\r\n",
      status, content_type, static_cast<unsigned>(body_len),
      extra_headers != nullptr ? extra_headers : "");
  if (header_len < 0 || static_cast<size_t>(header_len) >= sizeof(s_out)) {
    sendFixed500();  // header failed/truncated: never emit a partial header
    return;
  }
  size_t total = static_cast<size_t>(header_len);
  if (body_len > 0) {
    if (total + body_len > sizeof(s_out)) {
      sendFixed500();  // body would overflow the fixed buffer
      return;
    }
    memcpy(s_out + total, body, body_len);
    total += body_len;
  }
  writeAll(s_out, total);
}

void sendHtml(const char* status, const char* body) {
  sendResponse(status, "text/html; charset=utf-8", nullptr, body, strlen(body));
}

// Minimal truthful error bodies (no untrusted reflection).
void sendStatusOnly(const char* status) {
  int n = std::snprintf(s_body, sizeof(s_body), "%s\n", status);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(s_body)) {
    sendFixed500();
    return;
  }
  sendResponse(status, "text/plain; charset=utf-8", nullptr, s_body,
               static_cast<size_t>(n));
}

// GET / on a canonical portal Host: the concise setup form. Trusted local
// defaults (coordinates + toggles) are prefilled; the SSID is user-entered, the
// password field is always empty, and NO submitted data or stored PSK is ever
// reflected. The CSRF token is embedded as a hidden field (that is how the token
// reaches the legitimate client; it is not the WPA secret).
void sendRootForm() {
  const char* miles = s_content.use_miles ? " checked" : "";
  const char* rwys = s_content.show_runways ? " checked" : "";
  const char* csrf = s_content.csrf_token != nullptr ? s_content.csrf_token : "";
  int n = std::snprintf(
      s_body, sizeof(s_body),
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<title>Plane Radar setup</title></head><body>"
      "<h2>Plane Radar Wi-Fi setup</h2>"
      "<form method=POST action=/save>"
      "<p><label>Wi-Fi name (SSID)<br>"
      "<input name=ssid maxlength=32 required></label></p>"
      "<p><label>Wi-Fi password<br>"
      "<input name=psk type=password maxlength=63 autocomplete=off></label></p>"
      "<p>Leave the password blank to reuse the currently stored password, but "
      "only when the SSID above exactly matches the stored network. For any other "
      "or new SSID a blank password means an <b>open</b> network.</p>"
      "<p><label>Latitude<br>"
      "<input name=lat value=\"%.6f\" required></label></p>"
      "<p><label>Longitude<br>"
      "<input name=lon value=\"%.6f\" required></label></p>"
      "<p><label><input type=checkbox name=use_miles%s> "
      "Show distances in miles</label></p>"
      "<p><label><input type=checkbox name=show_runways%s> "
      "Show airport runways</label></p>"
      "<input type=hidden name=csrf value=\"%s\">"
      "<p><button type=submit>Save &amp; test</button></p>"
      "</form></body></html>",
      s_content.lat, s_content.lon, miles, rwys, csrf);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(s_body)) {
    sendFixed500();  // truncated form: never send a short Content-Length
    return;
  }
  sendResponse("200 OK", "text/html; charset=utf-8", nullptr, s_body,
               static_cast<size_t>(n));
}

// Accepted /save: honest "testing" page. The setup network drops immediately
// after; on failure it reopens with the SAME name/password shown on the panel.
void sendTestingPage() {
  sendHtml("200 OK",
           "<!doctype html><html><head><meta charset=utf-8></head><body>"
           "<h2>Testing your Wi-Fi\xE2\x80\xA6</h2>"
           "<p>The setup network is closing while the device connects. If it "
           "fails, the setup network reopens with the same name and password "
           "shown on the device screen.</p></body></html>");
}

// Fixed 302 to the constant portal authority (NEVER derived from the Host).
void sendRedirect() {
  char extra[64];
  const int n = std::snprintf(extra, sizeof(extra), "Location: %s\r\n",
                              core::kPortalRedirectLocation);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(extra)) {
    sendFixed500();
    return;
  }
  sendResponse("302 Found", "text/plain; charset=utf-8", extra, "", 0);
}

// Fixed captive-probe answer, deliberately DIFFERENT from the success sentinel
// each OS expects, so the captive portal is detected and opened. Token-free.
void sendCaptiveProbe() {
  sendHtml("200 OK",
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=\"0;url=http://192.168.4.1/\">"
           "</head><body><a href=\"http://192.168.4.1/\">Open Plane Radar "
           "setup</a></body></html>");
}

const char* errorStatus(core::HttpError err) {
  switch (err) {
    case core::HttpError::MethodNotAllowed:
      return "405 Method Not Allowed";
    case core::HttpError::UriTooLong:
      return "414 URI Too Long";
    case core::HttpError::HeaderLineTooLong:
    case core::HttpError::TooManyHeaderFields:
    case core::HttpError::HeaderSectionTooLarge:
      return "431 Request Header Fields Too Large";
    case core::HttpError::UnsupportedTransferEncoding:
      return "501 Not Implemented";
    case core::HttpError::LengthRequired:
      return "411 Length Required";
    case core::HttpError::PayloadTooLarge:
      return "413 Payload Too Large";
    case core::HttpError::UnsupportedMediaType:
      return "415 Unsupported Media Type";
    case core::HttpError::InvalidLimits:
      return "500 Internal Server Error";
    case core::HttpError::BadRequest:
    case core::HttpError::None:
    default:
      return "400 Bad Request";
  }
}

void respondToSave(const core::HttpRequest& req) {
  SaveOutcome outcome = SaveOutcome::BadRequest;
  if (s_content.on_save != nullptr) {
    outcome = s_content.on_save(req, s_content.on_save_ctx);
  }
  switch (outcome) {
    case SaveOutcome::Accepted:
      sendTestingPage();
      break;
    case SaveOutcome::Forbidden:
      sendStatusOnly("403 Forbidden");
      break;
    case SaveOutcome::Unprocessable:
      sendStatusOnly("422 Unprocessable Entity");
      break;
    case SaveOutcome::BadRequest:
    default:
      sendStatusOnly("400 Bad Request");
      break;
  }
}

void dispatchComplete() {
  const core::HttpRequest& req = core::httpRequestValue(s_parser);
  core::HttpRoute route = core::HttpRoute::None;
  switch (core::httpRouteResolve(req, &route)) {
    case core::RouteResult::Matched:
      switch (route) {
        case core::HttpRoute::Root:
          sendRootForm();
          break;
        case core::HttpRoute::Save:
          respondToSave(req);
          break;
        case core::HttpRoute::RedirectToPortal:
          sendRedirect();
          break;
        case core::HttpRoute::CaptiveGenerate204:
        case core::HttpRoute::CaptiveGen204:
        case core::HttpRoute::CaptiveHotspotDetect:
        case core::HttpRoute::CaptiveNcsi:
        case core::HttpRoute::CaptiveConnectTest:
        case core::HttpRoute::CaptiveCanonical:
        case core::HttpRoute::CaptiveSuccessTxt:
        case core::HttpRoute::CaptiveLibrarySuccess:
          sendCaptiveProbe();
          break;
        case core::HttpRoute::None:
        default:
          sendStatusOnly("404 Not Found");
          break;
      }
      break;
    case core::RouteResult::MethodNotAllowed:
      sendStatusOnly("405 Method Not Allowed");
      break;
    case core::RouteResult::Denied:
      sendStatusOnly("403 Forbidden");
      break;
    case core::RouteResult::NotFound:
    default:
      sendStatusOnly("404 Not Found");
      break;
  }
}

void closeClient() {
  s_client.stop();
  s_client_active = false;
  // Wipe the parsed request (may hold the home PSK) and the response/body
  // scratch (may hold the CSRF token) on every close.
  wipeWorkspaces();
}

}  // namespace

bool start(const PortalContent& content) {
  if (!core::httpLimitsValid(config::kPortalHttpLimits)) {
    return false;
  }
  // Bind the captive DNS first; a failure means we are not truly serving.
  const IPAddress ap_ip(content.ap_ip[0], content.ap_ip[1], content.ap_ip[2],
                        content.ap_ip[3]);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  if (!s_dns.start(config::kPortalDnsPort, "*", ap_ip)) {  // wildcard captive DNS
    return false;
  }
  // Bring up the WiFiServer (bound to the EXACT SoftAP IP with a one-client
  // backlog). Require operator bool() -> _listening true before we go active, so
  // a silent bind/listen failure never lets the caller believe a listener exists.
  s_server.begin();  // uses the constructor's SoftAP IP + port
  if (!s_server) {
    // begin() did not yield a listening server: close any half-open listen socket
    // with s_server.end() BEFORE tearing DNS down / returning false, so no stray
    // listen socket lingers on a failed bring-up.
    s_server.end();
    s_dns.stop();
    return false;
  }
  s_server.setNoDelay(true);  // set AFTER begin() (begin() resets _noDelay)
  s_content = content;
  // s_pump_fn / s_expired_fn are set once via their setters and preserved.
  core::httpRequestInit(&s_parser);
  s_client_active = false;
  s_active = true;
  return true;
}

// Pump one iteration: a captive DNS query, then advance the single client (accept
// via the AP-bound WiFiServer, incremental parse over lwip_recv, and on
// Complete/Error/timeout write one fixed response and close).
void pump() {
  if (!s_active) {
    return;
  }
  s_dns.processNextRequest();

  // Session deadline passed: do not accept or serve, and drop any in-flight
  // client, so the HTTP/DNS/AP surface is not held open past five minutes. The
  // owner (wifi_setup) tears the SoftAP down on its next deadline-preemption tick.
  if (sessionExpired()) {
    if (s_client_active) {
      closeClient();
    }
    return;
  }

  const uint32_t now = millis();
  if (!s_client_active) {
    WiFiClient incoming = s_server.accept();  // non-blocking; invalid when none
    if (!incoming) {
      return;
    }
    s_client = incoming;
    core::httpRequestInit(&s_parser);
    s_client_active = true;
    s_conn_started_ms = now;
    s_conn_last_byte_ms = now;
  }

  // Overall + idle timeouts: close/discard truthfully, no response.
  if (core::elapsedAtLeast(now, s_conn_started_ms,
                           config::kPortalHttpOverallTimeoutMs) ||
      core::elapsedAtLeast(now, s_conn_last_byte_ms,
                           config::kPortalHttpIdleTimeoutMs)) {
    closeClient();
    return;
  }

  const int fd = s_client.fd();
  if (fd < 0) {
    closeClient();
    return;
  }

  // Read directly off the socket with lwip_recv (bypassing WiFiClient's buffered
  // read), feed each chunk to the parser, then wipe the chunk immediately -- a
  // raw POST body chunk can carry the home PSK.
  uint8_t buf[64];
  while (true) {
    const int got = lwip_recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (got == 0) {
      closeClient();  // peer closed before a full request
      break;
    }
    if (got < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        break;  // no data pending: resume on a later pump
      }
      closeClient();  // real socket error
      break;
    }
    s_conn_last_byte_ms = millis();
    uint16_t consumed = 0;
    const core::HttpParseStatus st = core::httpRequestFeed(
        &s_parser, config::kPortalHttpLimits, buf,
        static_cast<uint16_t>(got), &consumed);
    core::secureZero(buf, sizeof(buf));  // wipe the raw chunk after feeding
    if (st == core::HttpParseStatus::Complete) {
      dispatchComplete();
      closeClient();
      break;
    }
    if (st == core::HttpParseStatus::Error) {
      sendStatusOnly(errorStatus(core::httpRequestError(s_parser)));
      closeClient();
      break;
    }
  }
  core::secureZero(buf, sizeof(buf));  // belt-and-suspenders on every exit path
}

void stop() {
  if (s_client_active) {
    closeClient();
  }
  s_server.end();  // WiFiServer::end() closes the listen socket, clears _listening
  s_dns.stop();
  s_active = false;
  // Wipe the workspaces and the content struct (which holds the CSRF token
  // pointer + trusted defaults) so nothing lingers after the portal is down.
  wipeWorkspaces();
  core::secureZero(&s_content, sizeof(s_content));
}

bool active() { return s_active; }

void setPumpFn(PumpFn fn) { s_pump_fn = fn; }

void setExpiredFn(ExpiredFn fn) { s_expired_fn = fn; }

}  // namespace services::portal
