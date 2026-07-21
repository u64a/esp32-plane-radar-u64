#include <unity.h>

#include <cstdint>
#include <cstring>

#include "core/http_host.h"
#include "core/http_request.h"
#include "core/http_router.h"

using core::HttpMethod;
using core::HttpRoute;
using core::PortalHostClass;
using core::RouteResult;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int R(RouteResult r) { return static_cast<int>(r); }
int RT(HttpRoute r) { return static_cast<int>(r); }

// Path/method resolution with a canonical portal Host (the default for the
// pre-host-gating behavioral tests: danger/404/405/case-sensitivity are all
// host-independent, and the two app routes reach Root/Save under a portal Host).
RouteResult res(HttpMethod m, const char* path, HttpRoute* out) {
  return core::httpRouteResolve(m, path, static_cast<uint16_t>(std::strlen(path)),
                                PortalHostClass::CanonicalPortal, out);
}

// Path/method resolution with an EXPLICIT Host classification.
RouteResult resH(HttpMethod m, const char* path, PortalHostClass host,
                 HttpRoute* out) {
  return core::httpRouteResolve(m, path, static_cast<uint16_t>(std::strlen(path)),
                                host, out);
}

// Parse a full request byte-by-byte and resolve it through the HttpRequest
// overload, so the router keys on the parser's own exact-match Host verdict.
RouteResult resolveParsed(const char* raw, HttpRoute* out) {
  core::HttpRequestParser p;
  core::httpRequestInit(&p);
  for (const char* c = raw; *c != '\0'; ++c) {
    core::httpRequestFeedByte(&p, core::kDefaultHttpLimits,
                              static_cast<uint8_t>(*c));
  }
  return core::httpRouteResolve(core::httpRequestValue(p), out);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_root_get_matched() {
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::Matched), R(res(HttpMethod::Get, "/", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::Root), RT(rt));
}

void test_save_post_matched() {
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::Matched),
                        R(res(HttpMethod::Post, "/save", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::Save), RT(rt));
}

void test_all_captive_probes_matched() {
  struct Case {
    const char* path;
    HttpRoute route;
  };
  const Case cases[] = {
      {"/generate_204", HttpRoute::CaptiveGenerate204},
      {"/gen_204", HttpRoute::CaptiveGen204},
      {"/hotspot-detect.html", HttpRoute::CaptiveHotspotDetect},
      {"/ncsi.txt", HttpRoute::CaptiveNcsi},
      {"/connecttest.txt", HttpRoute::CaptiveConnectTest},
      {"/canonical.html", HttpRoute::CaptiveCanonical},
      {"/success.txt", HttpRoute::CaptiveSuccessTxt},
      {"/library/test/success.html", HttpRoute::CaptiveLibrarySuccess},
  };
  for (const Case& c : cases) {
    HttpRoute rt = HttpRoute::None;
    TEST_ASSERT_EQUAL_INT(R(RouteResult::Matched),
                          R(res(HttpMethod::Get, c.path, &rt)));
    TEST_ASSERT_EQUAL_INT(RT(c.route), RT(rt));
  }
}

void test_wrong_method_405() {
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::MethodNotAllowed),
                        R(res(HttpMethod::Post, "/", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::None), RT(rt));
  TEST_ASSERT_EQUAL_INT(R(RouteResult::MethodNotAllowed),
                        R(res(HttpMethod::Get, "/save", &rt)));
  TEST_ASSERT_EQUAL_INT(R(RouteResult::MethodNotAllowed),
                        R(res(HttpMethod::Post, "/generate_204", &rt)));
}

void test_unknown_path_not_found() {
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::NotFound),
                        R(res(HttpMethod::Get, "/nope", &rt)));
  TEST_ASSERT_EQUAL_INT(R(RouteResult::NotFound),
                        R(res(HttpMethod::Get, "/save.html", &rt)));
}

void test_case_sensitive_allow_paths() {
  HttpRoute rt = HttpRoute::None;
  // Allowed routes match case-sensitively; a case variant is simply NotFound.
  TEST_ASSERT_EQUAL_INT(R(RouteResult::NotFound),
                        R(res(HttpMethod::Post, "/Save", &rt)));
  TEST_ASSERT_EQUAL_INT(R(RouteResult::NotFound),
                        R(res(HttpMethod::Get, "/GENERATE_204", &rt)));
}

void test_danger_routes_denied() {
  const char* danger[] = {"/update", "/u",    "/erase",  "/reset", "/reboot",
                          "/restart", "/info", "/fwlink", "/setwifisave"};
  HttpRoute rt = HttpRoute::None;
  for (const char* d : danger) {
    TEST_ASSERT_EQUAL_INT(R(RouteResult::Denied), R(res(HttpMethod::Get, d, &rt)));
    TEST_ASSERT_EQUAL_INT(R(RouteResult::Denied),
                          R(res(HttpMethod::Post, d, &rt)));
  }
}

void test_danger_routes_case_insensitive_denied() {
  const char* variants[] = {"/UPDATE", "/Update", "/uPdAtE",
                            "/ERASE",  "/Reboot", "/SetWifiSave"};
  HttpRoute rt = HttpRoute::None;
  for (const char* v : variants) {
    TEST_ASSERT_EQUAL_INT(R(RouteResult::Denied), R(res(HttpMethod::Get, v, &rt)));
  }
}

void test_resolve_request_helper_and_query_ignored() {
  // The router keys on path only; a captive probe with a query still matches.
  core::HttpRequestParser p;
  core::httpRequestInit(&p);
  const char* good = "GET /gen_204?x=1 HTTP/1.1\r\n\r\n";
  for (const char* c = good; *c; ++c) {
    core::httpRequestFeedByte(&p, core::kDefaultHttpLimits,
                              static_cast<uint8_t>(*c));
  }
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::Matched),
                        R(core::httpRouteResolve(core::httpRequestValue(p), &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::CaptiveGen204), RT(rt));
}

// --- DNS-rebinding Host gating ----------------------------------------------

void test_noncanonical_get_root_redirects_never_form() {
  // A rebound attacker origin sends its OWN Host on GET /: it must resolve to the
  // fixed portal redirect, never the CSRF-bearing form.
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Matched),
      R(resH(HttpMethod::Get, "/", PortalHostClass::NonCanonical, &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::RedirectToPortal), RT(rt));
  // End-to-end through the parser with an attacker authority.
  rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Matched),
      R(resolveParsed("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::RedirectToPortal), RT(rt));
  TEST_ASSERT_NOT_EQUAL(RT(HttpRoute::Root), RT(rt));
}

void test_noncanonical_post_save_denied_never_save() {
  // A rebound attacker POST /save must be denied outright: it never reaches Save.
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Denied),
      R(resH(HttpMethod::Post, "/save", PortalHostClass::NonCanonical, &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::None), RT(rt));
  rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Denied),
      R(resolveParsed("POST /save HTTP/1.1\r\nHost: evil.example\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 0\r\n\r\n",
                      &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::None), RT(rt));
}

void test_canonical_authorities_reach_form_and_save() {
  // Both canonical spellings (with and without the default :80) reach the app
  // routes, proven end-to-end through the parser's own Host classification.
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Matched),
      R(resolveParsed("GET / HTTP/1.1\r\nHost: 192.168.4.1\r\n\r\n", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::Root), RT(rt));
  rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Matched),
      R(resolveParsed("GET / HTTP/1.1\r\nHost: 192.168.4.1:80\r\n\r\n", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::Root), RT(rt));
  rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Matched),
      R(resolveParsed("POST /save HTTP/1.1\r\nHost: 192.168.4.1\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 0\r\n\r\n",
                      &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::Save), RT(rt));
}

void test_probe_routes_ignore_host() {
  // Captive probes deliberately carry external OS hostnames and must still match
  // their fixed routes regardless of Host classification.
  struct Case {
    const char* path;
    HttpRoute route;
  };
  const Case cases[] = {
      {"/generate_204", HttpRoute::CaptiveGenerate204},
      {"/gen_204", HttpRoute::CaptiveGen204},
      {"/hotspot-detect.html", HttpRoute::CaptiveHotspotDetect},
      {"/ncsi.txt", HttpRoute::CaptiveNcsi},
      {"/connecttest.txt", HttpRoute::CaptiveConnectTest},
      {"/canonical.html", HttpRoute::CaptiveCanonical},
      {"/success.txt", HttpRoute::CaptiveSuccessTxt},
      {"/library/test/success.html", HttpRoute::CaptiveLibrarySuccess},
  };
  for (const Case& c : cases) {
    HttpRoute rt = HttpRoute::None;
    TEST_ASSERT_EQUAL_INT(
        R(RouteResult::Matched),
        R(resH(HttpMethod::Get, c.path, PortalHostClass::NonCanonical, &rt)));
    TEST_ASSERT_EQUAL_INT(RT(c.route), RT(rt));
  }
}

void test_host_less_http_1_0_cannot_reach_form_or_save() {
  // A Host-less HTTP/1.0 request is classed noncanonical: GET / redirects and
  // POST /save is denied, so it can neither retrieve nor submit the form.
  HttpRoute rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(R(RouteResult::Matched),
                        R(resolveParsed("GET / HTTP/1.0\r\n\r\n", &rt)));
  TEST_ASSERT_EQUAL_INT(RT(HttpRoute::RedirectToPortal), RT(rt));
  rt = HttpRoute::None;
  TEST_ASSERT_EQUAL_INT(
      R(RouteResult::Denied),
      R(resolveParsed("POST /save HTTP/1.0\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: 0\r\n\r\n",
                      &rt)));
}

void test_redirect_target_is_a_fixed_constant() {
  // The redirect target is portal policy, never derived from the request Host.
  TEST_ASSERT_EQUAL_STRING("http://192.168.4.1/", core::kPortalRedirectLocation);
  TEST_ASSERT_EQUAL_STRING("192.168.4.1", core::kPortalAuthority);
  TEST_ASSERT_EQUAL_STRING("192.168.4.1:80", core::kPortalAuthorityWithPort);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_root_get_matched);
  RUN_TEST(test_save_post_matched);
  RUN_TEST(test_all_captive_probes_matched);
  RUN_TEST(test_wrong_method_405);
  RUN_TEST(test_unknown_path_not_found);
  RUN_TEST(test_case_sensitive_allow_paths);
  RUN_TEST(test_danger_routes_denied);
  RUN_TEST(test_danger_routes_case_insensitive_denied);
  RUN_TEST(test_resolve_request_helper_and_query_ignored);
  RUN_TEST(test_noncanonical_get_root_redirects_never_form);
  RUN_TEST(test_noncanonical_post_save_denied_never_save);
  RUN_TEST(test_canonical_authorities_reach_form_and_save);
  RUN_TEST(test_probe_routes_ignore_host);
  RUN_TEST(test_host_less_http_1_0_cannot_reach_form_or_save);
  RUN_TEST(test_redirect_target_is_a_fixed_constant);
  return UNITY_END();
}
