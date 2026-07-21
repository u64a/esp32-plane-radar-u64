#include <unity.h>

#include "core/network_work_intent.h"

using core::NetworkWorkIntent;
using core::NetworkWorkIntentState;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int IN(NetworkWorkIntent i) { return static_cast<int>(i); }

NetworkWorkIntentState fresh() {
  NetworkWorkIntentState s;
  core::networkWorkIntentInit(&s);
  return s;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_init_is_none() {
  NetworkWorkIntentState s = fresh();
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None), IN(core::pendingIntent(s)));
  // Consuming an empty latch never yields an action, quiesced or not.
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, false)));
}

void test_configure_deferred_until_quiescence() {
  NetworkWorkIntentState s = fresh();
  core::requestConfigure(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::pendingIntent(s)));

  // Not quiesced: no configure action is released; the intent stays pending.
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, false)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::pendingIntent(s)));

  // Quiesced: released exactly once (one-shot).
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::pendingIntent(s)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
}

void test_erase_deferred_until_quiescence() {
  NetworkWorkIntentState s = fresh();
  core::requestErase(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::pendingIntent(s)));
  // No erase action before quiescence.
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, false)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::pendingIntent(s)));
  // Released once quiescent, one-shot.
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
}

void test_configure_is_idempotent() {
  NetworkWorkIntentState s = fresh();
  core::requestConfigure(&s);
  core::requestConfigure(&s);
  core::requestConfigure(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::pendingIntent(s)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
}

void test_erase_supersedes_configure() {
  NetworkWorkIntentState s = fresh();
  core::requestConfigure(&s);
  core::requestErase(&s);  // erase outranks configure
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::pendingIntent(s)));
  // A later configure must NOT override the pending erase.
  core::requestConfigure(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::pendingIntent(s)));
  // And it releases as an erase.
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::consumeIntent(&s, true)));
}

void test_configure_never_downgrades_erase_even_after_many_requests() {
  NetworkWorkIntentState s = fresh();
  core::requestErase(&s);
  for (int i = 0; i < 5; ++i) {
    core::requestConfigure(&s);
    TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                          IN(core::pendingIntent(s)));
  }
  core::requestErase(&s);  // idempotent erase
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::pendingIntent(s)));
}

void test_consumption_is_one_shot_then_a_new_request_latches_again() {
  NetworkWorkIntentState s = fresh();
  core::requestConfigure(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
  // A brand-new request re-latches and can be consumed once more.
  core::requestErase(&s);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::consumeIntent(&s, true)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));
}

void test_null_state_is_safe() {
  core::networkWorkIntentInit(nullptr);
  core::requestConfigure(nullptr);
  core::requestErase(nullptr);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(nullptr, true)));
}

void test_configure_superseded_by_erase_while_waiting_releases_only_erase() {
  // Mirrors the worker deferral handshake: Configure is latched, the worker is
  // not yet quiesced across several wifiLoop ticks, then Erase is confirmed and
  // supersedes. Once quiescence is proven, ONLY Erase releases -- Configure's
  // credential snapshot is never taken.
  NetworkWorkIntentState s = fresh();
  core::requestConfigure(&s);
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                          IN(core::consumeIntent(&s, false)));  // still winding down
    TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Configure),
                          IN(core::pendingIntent(s)));
  }
  core::requestErase(&s);  // supersede while still waiting
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase), IN(core::pendingIntent(s)));
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, false)));  // still not quiesced
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase),
                        IN(core::consumeIntent(&s, true)));  // quiesced -> Erase
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::None),
                        IN(core::consumeIntent(&s, true)));  // one-shot
}

void test_repeated_wait_ticks_never_release_until_quiescence() {
  // Many not-quiesced ticks (repeated wifiLoop passes while the worker is still
  // Running/ResultReady) must NEVER release; exactly the first quiesced tick does.
  NetworkWorkIntentState s = fresh();
  core::requestErase(&s);
  int releases = 0;
  for (int i = 0; i < 10; ++i) {
    releases += (core::consumeIntent(&s, false) != NetworkWorkIntent::None) ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(0, releases);
  TEST_ASSERT_EQUAL_INT(IN(NetworkWorkIntent::Erase), IN(core::pendingIntent(s)));

  int quiesced_releases = 0;
  for (int i = 0; i < 5; ++i) {
    quiesced_releases +=
        (core::consumeIntent(&s, true) != NetworkWorkIntent::None) ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(1, quiesced_releases);  // one-shot on the first quiesced tick
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_init_is_none);
  RUN_TEST(test_configure_deferred_until_quiescence);
  RUN_TEST(test_erase_deferred_until_quiescence);
  RUN_TEST(test_configure_is_idempotent);
  RUN_TEST(test_erase_supersedes_configure);
  RUN_TEST(test_configure_never_downgrades_erase_even_after_many_requests);
  RUN_TEST(test_consumption_is_one_shot_then_a_new_request_latches_again);
  RUN_TEST(test_configure_superseded_by_erase_while_waiting_releases_only_erase);
  RUN_TEST(test_repeated_wait_ticks_never_release_until_quiescence);
  RUN_TEST(test_null_state_is_safe);
  return UNITY_END();
}
