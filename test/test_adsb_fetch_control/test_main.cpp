#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "services/adsb_fetch.h"
#include "services/adsb_snapshot_store.h"
#include "services/adsb_transport.h"

#include "../support/adsb_stream_fakes.h"

using namespace services::adsb;
using test_support::CountingClock;
using test_support::ScriptedByteSource;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

// Cooperative-cancel context shared by a FetchControl's idle pump and its
// cancellation predicate: onIdle() increments idle_calls, and cancel() reports
// cancelled once idle_calls reaches cancel_after. A huge cancel_after never
// cancels (the "existing path unchanged" control).
struct CancelCtx {
  int idle_calls = 0;
  int cancel_after = 1 << 30;
};

void countIdle(void* ctx) { static_cast<CancelCtx*>(ctx)->idle_calls += 1; }

bool cancelPred(void* ctx) {
  const CancelCtx* c = static_cast<CancelCtx*>(ctx);
  return c->idle_calls >= c->cancel_after;
}

FetchControl makeControl(CancelCtx* ctx, bool with_cancel) {
  FetchControl control{};
  control.idle = &countIdle;
  control.idle_ctx = ctx;
  if (with_cancel) {
    control.cancel = &cancelPred;
    control.cancel_ctx = ctx;
  }
  return control;
}

struct Scratch {
  float distances[kMaxAircraft];
  uint16_t ordinals[kMaxAircraft];
  char object_buffer[kObjectBufferBytes];
  unsigned char arena[kJsonArenaBytes];
  uint8_t scratch[512];
  char line[1024];
};

Scratch g_s;

FetchWorkspace ws() {
  FetchWorkspace w{};
  w.http = HttpWorkspace{g_s.scratch, sizeof(g_s.scratch), g_s.line,
                         sizeof(g_s.line)};
  w.object_buffer = g_s.object_buffer;
  w.object_capacity = sizeof(g_s.object_buffer);
  w.arena = g_s.arena;
  w.arena_size = sizeof(g_s.arena);
  w.distances = g_s.distances;
  w.ordinals = g_s.ordinals;
  return w;
}

std::string validBody(int aircraft) {
  std::string j = "{\"ac\":[";
  for (int i = 0; i < aircraft; ++i) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s{\"lat\":0,\"lon\":%f}", i ? "," : "",
                  0.001 * (i + 1));
    j += buf;
  }
  j += "]}";
  return j;
}

std::string resp(const std::string& body) {
  char hdr[128];
  std::snprintf(
      hdr, sizeof(hdr),
      "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
      body.size());
  return std::string(hdr) + body;
}

// Run one fetch driven entirely through a FetchControl (the exact idle/cancel
// plumbing the ESP worker path uses). The clock never advances, so no timeout
// fires and only a cooperative cancel can abort the decode. Fragments the stream
// and injects WouldBlock so the decoder's idle/refill loop runs many times.
FetchResult runControlled(const std::string& raw, size_t frag,
                          const FetchControl& control, AircraftSnapshot* out) {
  ScriptedByteSource src(raw, frag, ScriptedByteSource::Terminal::End,
                         /*inject_wouldblock=*/true);
  CountingClock clock;  // static clock: no overall/inactivity timeout can fire
  FetchControlIdle idle(control);
  const HttpDeadlines dl{60000, 30000};
  FetchWorkspace w = ws();
  return runFetch(src, clock, idle, 0.0, 0.0,
                  ParseOptions{kDefaultParseLimits, false}, kDefaultHttpLimits,
                  dl, w, *out, 1);
}

// File-scope wiring so a SnapshotStore FetchFn (which only carries a void* ctx)
// can drive a controlled fetch over a scripted response.
std::string g_store_response;
size_t g_store_frag = 8;

FetchResult storeControlledFetch(const FetchRequest& /*request*/,
                                 AircraftSnapshot& out, void* ctx) {
  const FetchControl* control = static_cast<const FetchControl*>(ctx);
  return runControlled(g_store_response, g_store_frag, *control, &out);
}

int OC(FetchOutcome o) { return static_cast<int>(o); }

}  // namespace

void setUp() { std::memset(&g_s, 0, sizeof(g_s)); }
void tearDown() {}

// --- FetchControl / FetchControlIdle seam --------------------------------------

void test_fetch_control_idle_routes_onidle_and_cancel() {
  CancelCtx ctx;
  ctx.cancel_after = 3;
  FetchControl control = makeControl(&ctx, /*with_cancel=*/true);
  FetchControlIdle idle(control);

  TEST_ASSERT_FALSE(idle.cancelled());
  idle.onIdle();
  TEST_ASSERT_FALSE(idle.cancelled());  // 1 < 3
  idle.onIdle();
  TEST_ASSERT_FALSE(idle.cancelled());  // 2 < 3
  idle.onIdle();
  TEST_ASSERT_TRUE(idle.cancelled());  // 3 >= 3
  TEST_ASSERT_EQUAL_INT(3, ctx.idle_calls);
}

void test_fetch_control_null_callbacks_are_safe() {
  FetchControl control{};  // all null
  FetchControlIdle idle(control);
  idle.onIdle();  // no-op, must not crash
  TEST_ASSERT_FALSE(idle.cancelled());
  TEST_ASSERT_FALSE(fetchControlCancelled(control));
}

void test_fetch_control_cancelled_helper() {
  CancelCtx ctx;
  // No predicate installed: never cancelled regardless of the context.
  FetchControl no_cancel = makeControl(&ctx, /*with_cancel=*/false);
  ctx.idle_calls = 100;
  TEST_ASSERT_FALSE(fetchControlCancelled(no_cancel));

  FetchControl with_cancel = makeControl(&ctx, /*with_cancel=*/true);
  ctx.cancel_after = 1;  // idle_calls (100) >= 1
  TEST_ASSERT_TRUE(fetchControlCancelled(with_cancel));
}

void test_base_idle_handler_defaults_to_not_cancelled() {
  // The default IdleHandler::cancelled() keeps every existing fake source-
  // compatible and never cancels.
  test_support::CountingClock clock;
  test_support::AdvancingIdle idle(&clock, 1);
  IdleHandler& base = idle;
  TEST_ASSERT_FALSE(base.cancelled());
}

// --- cancellation aborts the HTTP decode loop ---------------------------------

void test_no_cancel_decodes_ok_baseline() {
  // The SAME fragmented + WouldBlock stream decodes cleanly to Ok when nothing
  // cancels, proving the abort below is caused only by the cancel predicate.
  CancelCtx ctx;  // cancel_after huge: never cancels
  FetchControl control = makeControl(&ctx, /*with_cancel=*/false);
  AircraftSnapshot out{};
  const FetchResult fr = runControlled(resp(validBody(5)), 8, control, &out);
  TEST_ASSERT_EQUAL_INT(OC(FetchOutcome::Ok), OC(fr.outcome));
  TEST_ASSERT_EQUAL_UINT16(5, fr.aircraft_count);
  TEST_ASSERT_EQUAL_UINT16(5, out.count);
  TEST_ASSERT_TRUE(ctx.idle_calls > 0);  // the idle/refill loop actually ran
}

void test_cancel_aborts_decode_without_ok() {
  // Cancel partway through the decode: it aborts as a transport-like failure
  // (never Ok), and no aircraft count is stamped into the snapshot.
  CancelCtx ctx;
  ctx.cancel_after = 6;
  FetchControl control = makeControl(&ctx, /*with_cancel=*/true);
  AircraftSnapshot out{};
  out.count = 4242;  // poison: a real abort must not overwrite this with a count
  const FetchResult fr = runControlled(resp(validBody(20)), 8, control, &out);

  TEST_ASSERT_NOT_EQUAL(OC(FetchOutcome::Ok), OC(fr.outcome));
  TEST_ASSERT_EQUAL_INT(OC(FetchOutcome::TransportFailure), OC(fr.outcome));
  TEST_ASSERT_EQUAL_UINT16(0, fr.aircraft_count);  // no partial success reported
  TEST_ASSERT_EQUAL_UINT16(4242, out.count);       // count never stamped on abort
}

void test_cancel_maps_to_transient_poll_outcome() {
  // The transport-like abort classifies as a transient poll outcome (retry
  // briefly), never Permanent/Success -- the worker envelope carries the real
  // cancelled flag separately.
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(core::PollOutcome::Transient),
      static_cast<int>(pollOutcomeFor(FetchOutcome::TransportFailure)));
}

// --- cancellation never publishes a partial candidate -------------------------

void test_cancelled_store_fetch_does_not_publish() {
  SnapshotStore store;
  g_store_frag = 8;

  // 1. Publish a known-good snapshot so the active slot has real data.
  g_store_response = resp(validBody(3));
  CancelCtx ok_ctx;  // never cancels
  FetchControl ok_control = makeControl(&ok_ctx, /*with_cancel=*/false);
  CandidateResult good =
      store.fetchCandidate(FetchRequest{0.0, 0.0, 10.0f, 1}, &storeControlledFetch,
                           &ok_control);
  TEST_ASSERT_EQUAL_INT(OC(FetchOutcome::Ok), OC(good.fetch.outcome));
  TEST_ASSERT_TRUE(good.handle.valid());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishResult::Published),
                        static_cast<int>(store.publishCandidate(good.handle, 1)));
  TEST_ASSERT_EQUAL_size_t(3, store.aircraftCount());
  TEST_ASSERT_EQUAL_UINT32(1, store.publishedRevision());

  // 2. A cancelled fetch aborts mid-decode: the store auto-discards the candidate
  //    and the published (active) snapshot is preserved byte-for-byte.
  g_store_response = resp(validBody(20));
  CancelCtx cancel_ctx;
  cancel_ctx.cancel_after = 5;
  FetchControl cancel_control = makeControl(&cancel_ctx, /*with_cancel=*/true);
  CandidateResult aborted = store.fetchCandidate(
      FetchRequest{0.0, 0.0, 10.0f, 2}, &storeControlledFetch, &cancel_control);

  TEST_ASSERT_NOT_EQUAL(OC(FetchOutcome::Ok), OC(aborted.fetch.outcome));
  TEST_ASSERT_FALSE(aborted.handle.valid());
  TEST_ASSERT_FALSE(store.candidateOutstanding());  // nothing to publish
  TEST_ASSERT_EQUAL_size_t(3, store.aircraftCount());     // unchanged
  TEST_ASSERT_EQUAL_UINT32(1, store.publishedRevision());  // unchanged

  // 3. Publishing the aborted handle is rejected (nothing is outstanding) and
  //    still preserves the active snapshot byte-for-byte.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishResult::NoCandidate),
                        static_cast<int>(store.publishCandidate(aborted.handle, 2)));
  TEST_ASSERT_EQUAL_size_t(3, store.aircraftCount());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fetch_control_idle_routes_onidle_and_cancel);
  RUN_TEST(test_fetch_control_null_callbacks_are_safe);
  RUN_TEST(test_fetch_control_cancelled_helper);
  RUN_TEST(test_base_idle_handler_defaults_to_not_cancelled);
  RUN_TEST(test_no_cancel_decodes_ok_baseline);
  RUN_TEST(test_cancel_aborts_decode_without_ok);
  RUN_TEST(test_cancel_maps_to_transient_poll_outcome);
  RUN_TEST(test_cancelled_store_fetch_does_not_publish);
  return UNITY_END();
}
