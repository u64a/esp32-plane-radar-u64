#include <unity.h>

#include <cstring>

#include "core/wifi_field.h"

// Boundary tests for core::wifiFieldCopy, the pure copy backing the ESP
// wifi_config_t ssid[32]/password[64] fields. The critical case is a maximal
// 32-byte SSID, which must fill the whole field with NO terminator (the WiFi
// driver derives length via strnlen), so it is not silently truncated to 31.

void setUp() {}
void tearDown() {}

namespace {
constexpr unsigned char kFill = 0xAB;
}

void test_short_span_is_nul_terminated() {
  unsigned char dst[32];
  memset(dst, kFill, sizeof(dst));
  const size_t n = core::wifiFieldCopy(dst, sizeof(dst), "abc", 3);
  TEST_ASSERT_EQUAL_UINT32(3, n);
  TEST_ASSERT_EQUAL_UINT8('a', dst[0]);
  TEST_ASSERT_EQUAL_UINT8('c', dst[2]);
  TEST_ASSERT_EQUAL_UINT8(0, dst[3]);  // terminated (room remained)
}

void test_full_32_byte_ssid_fills_field_without_terminator() {
  unsigned char dst[32];
  memset(dst, kFill, sizeof(dst));
  const char ssid[32] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
                         'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
                         'W', 'X', 'Y', 'Z', '0', '1', '2', '3', '4', '5'};
  const size_t n = core::wifiFieldCopy(dst, sizeof(dst), ssid, 32);
  TEST_ASSERT_EQUAL_UINT32(32, n);              // all 32 copied (not 31)
  TEST_ASSERT_EQUAL_MEMORY(ssid, dst, 32);      // no character dropped
  TEST_ASSERT_EQUAL_UINT32(32, strnlen(reinterpret_cast<char*>(dst), 32));
}

void test_31_byte_span_is_terminated_at_31() {
  unsigned char dst[32];
  memset(dst, kFill, sizeof(dst));
  char src[31];
  memset(src, 'x', sizeof(src));
  const size_t n = core::wifiFieldCopy(dst, sizeof(dst), src, 31);
  TEST_ASSERT_EQUAL_UINT32(31, n);
  TEST_ASSERT_EQUAL_UINT8('x', dst[30]);
  TEST_ASSERT_EQUAL_UINT8(0, dst[31]);  // terminated in the last byte
}

void test_over_long_span_truncates_to_capacity_without_terminator() {
  unsigned char dst[32];
  memset(dst, kFill, sizeof(dst));
  char src[40];
  memset(src, 'y', sizeof(src));
  const size_t n = core::wifiFieldCopy(dst, sizeof(dst), src, 40);
  TEST_ASSERT_EQUAL_UINT32(32, n);  // capped at capacity
  for (int i = 0; i < 32; ++i) {
    TEST_ASSERT_EQUAL_UINT8('y', dst[i]);  // whole field filled, no NUL
  }
}

void test_empty_and_null_span_terminate_at_zero() {
  unsigned char dst[32];
  memset(dst, kFill, sizeof(dst));
  TEST_ASSERT_EQUAL_UINT32(0, core::wifiFieldCopy(dst, sizeof(dst), "ignored", 0));
  TEST_ASSERT_EQUAL_UINT8(0, dst[0]);

  memset(dst, kFill, sizeof(dst));
  TEST_ASSERT_EQUAL_UINT32(0, core::wifiFieldCopy(dst, sizeof(dst), nullptr, 5));
  TEST_ASSERT_EQUAL_UINT8(0, dst[0]);
}

void test_password_63_byte_span_fits_with_terminator() {
  unsigned char dst[64];  // wifi_config_t.sta.password
  memset(dst, kFill, sizeof(dst));
  char src[63];
  memset(src, 'p', sizeof(src));
  const size_t n = core::wifiFieldCopy(dst, sizeof(dst), src, 63);
  TEST_ASSERT_EQUAL_UINT32(63, n);
  TEST_ASSERT_EQUAL_UINT8('p', dst[62]);
  TEST_ASSERT_EQUAL_UINT8(0, dst[63]);  // 63-char WPA2 psk + NUL fits in 64
}

void test_guards_reject_null_or_zero_capacity() {
  unsigned char dst[4];
  TEST_ASSERT_EQUAL_UINT32(0, core::wifiFieldCopy(nullptr, 4, "ab", 2));
  TEST_ASSERT_EQUAL_UINT32(0, core::wifiFieldCopy(dst, 0, "ab", 2));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_short_span_is_nul_terminated);
  RUN_TEST(test_full_32_byte_ssid_fills_field_without_terminator);
  RUN_TEST(test_31_byte_span_is_terminated_at_31);
  RUN_TEST(test_over_long_span_truncates_to_capacity_without_terminator);
  RUN_TEST(test_empty_and_null_span_terminate_at_zero);
  RUN_TEST(test_password_63_byte_span_fits_with_terminator);
  RUN_TEST(test_guards_reject_null_or_zero_capacity);
  return UNITY_END();
}
