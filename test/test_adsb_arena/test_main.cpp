#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "services/adsb_arena.h"

using services::adsb::BoundedArena;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_allocate_and_free_round_trip() {
  alignas(16) unsigned char buf[1024];
  BoundedArena arena(buf, sizeof(buf));
  void* a = arena.allocate(100);
  void* b = arena.allocate(200);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_TRUE(a != b);
  arena.deallocate(a);
  arena.deallocate(b);
  TEST_ASSERT_EQUAL_UINT32(0, arena.bytesInUse());
}

void test_allocations_are_max_aligned() {
  alignas(16) unsigned char buf[1024];
  BoundedArena arena(buf, sizeof(buf));
  for (int i = 0; i < 8; ++i) {
    void* p = arena.allocate(1 + i * 7);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(
        0, reinterpret_cast<uintptr_t>(p) % alignof(max_align_t));
  }
}

void test_exhaustion_returns_null_then_recovers_after_free() {
  alignas(16) unsigned char buf[512];
  BoundedArena arena(buf, sizeof(buf));
  std::vector<void*> blocks;
  for (;;) {
    void* p = arena.allocate(48);
    if (p == nullptr) break;
    blocks.push_back(p);
  }
  TEST_ASSERT_TRUE(!blocks.empty());
  TEST_ASSERT_NULL(arena.allocate(48));
  for (void* p : blocks) arena.deallocate(p);
  TEST_ASSERT_EQUAL_UINT32(0, arena.bytesInUse());
  // Coalescing restored one large free region.
  void* big = arena.allocate(400);
  TEST_ASSERT_NOT_NULL(big);
}

void test_reallocate_grow_and_shrink_preserve_contents() {
  alignas(16) unsigned char buf[1024];
  BoundedArena arena(buf, sizeof(buf));
  char* s = static_cast<char*>(arena.allocate(16));
  std::strcpy(s, "preserve");
  char* grown = static_cast<char*>(arena.reallocate(s, 400));
  TEST_ASSERT_NOT_NULL(grown);
  TEST_ASSERT_EQUAL_STRING("preserve", grown);
  char* shrunk = static_cast<char*>(arena.reallocate(grown, 8));
  TEST_ASSERT_NOT_NULL(shrunk);
  TEST_ASSERT_EQUAL_INT(0, std::strncmp(shrunk, "preserve", 5));
  arena.deallocate(shrunk);
}

void test_reallocate_null_and_zero_semantics() {
  alignas(16) unsigned char buf[512];
  BoundedArena arena(buf, sizeof(buf));
  void* p = arena.reallocate(nullptr, 32);  // behaves like allocate
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_NULL(arena.reallocate(p, 0));  // behaves like free
  TEST_ASSERT_EQUAL_UINT32(0, arena.bytesInUse());
}

void test_reallocate_failure_keeps_original_block() {
  alignas(16) unsigned char buf[512];
  BoundedArena arena(buf, sizeof(buf));
  char* keep = static_cast<char*>(arena.allocate(48));
  std::strcpy(keep, "intact");
  std::vector<void*> filler;
  for (;;) {
    void* q = arena.allocate(32);
    if (q == nullptr) break;
    filler.push_back(q);
  }
  TEST_ASSERT_NULL(arena.reallocate(keep, 900));
  TEST_ASSERT_EQUAL_STRING("intact", keep);
}

void test_zero_size_buffer_allocations_fail_cleanly() {
  BoundedArena arena(nullptr, 0);
  TEST_ASSERT_NULL(arena.allocate(1));
  TEST_ASSERT_EQUAL_UINT32(0, arena.capacity());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_allocate_and_free_round_trip);
  RUN_TEST(test_allocations_are_max_aligned);
  RUN_TEST(test_exhaustion_returns_null_then_recovers_after_free);
  RUN_TEST(test_reallocate_grow_and_shrink_preserve_contents);
  RUN_TEST(test_reallocate_null_and_zero_semantics);
  RUN_TEST(test_reallocate_failure_keeps_original_block);
  RUN_TEST(test_zero_size_buffer_allocations_fail_cleanly);
  return UNITY_END();
}
