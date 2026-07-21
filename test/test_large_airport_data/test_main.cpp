// Native unit tests for data/large_airports_data.cpp.
// Verifies compile-time and runtime invariants; deterministic and fast (no I/O).
#include <unity.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "data/large_airports.h"

using namespace data::large_airports;

void setUp()    {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Compile-time: std::extent equals constants
// ---------------------------------------------------------------------------
static_assert(std::extent<decltype(kAirports)>::value == kAirportCount,
              "kAirports array size must equal kAirportCount");
static_assert(std::extent<decltype(kRunways)>::value == kRunwayCount,
              "kRunways array size must equal kRunwayCount");

// ---------------------------------------------------------------------------
// T01: actual airport count equals kAirportCount
// ---------------------------------------------------------------------------
void test_airport_count_matches_constant() {
    constexpr size_t extent = std::extent<decltype(kAirports)>::value;
    TEST_ASSERT_EQUAL_UINT(kAirportCount, extent);
    TEST_ASSERT_EQUAL_UINT(1166, kAirportCount);
}

// ---------------------------------------------------------------------------
// T02: actual runway count equals kRunwayCount
// ---------------------------------------------------------------------------
void test_runway_count_matches_constant() {
    constexpr size_t extent = std::extent<decltype(kRunways)>::value;
    TEST_ASSERT_EQUAL_UINT(kRunwayCount, extent);
    TEST_ASSERT_EQUAL_UINT(1706, kRunwayCount);
}

// ---------------------------------------------------------------------------
// T03: all airport idents are exactly 4 characters (null-terminated in [5])
// ---------------------------------------------------------------------------
void test_airport_idents_are_4_chars() {
    for (size_t i = 0; i < kAirportCount; ++i) {
        const char* id = kAirports[i].ident;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(4, std::strlen(id),
            "airport ident should be 4 chars");
    }
}

// ---------------------------------------------------------------------------
// T04: airport idents are globally unique
// ---------------------------------------------------------------------------
void test_airport_idents_unique() {
    // Walk sorted array; duplicates would be adjacent.
    for (size_t i = 1; i < kAirportCount; ++i) {
        int cmp = std::strcmp(kAirports[i-1].ident, kAirports[i].ident);
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, cmp,
            "adjacent idents must be in strict ascending order (uniqueness)");
    }
}

// ---------------------------------------------------------------------------
// T05: airport idents are in strict ascending lexicographic order
// ---------------------------------------------------------------------------
void test_airport_idents_sorted() {
    for (size_t i = 1; i < kAirportCount; ++i) {
        int cmp = std::strcmp(kAirports[i-1].ident, kAirports[i].ident);
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, cmp,
            "airports must be sorted in strict ascending ICAO order");
    }
}

// ---------------------------------------------------------------------------
// T06: all airport latitudes in [-90*1e7, 90*1e7]
// ---------------------------------------------------------------------------
void test_airport_lat_range() {
    for (size_t i = 0; i < kAirportCount; ++i) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(-900000000, kAirports[i].lat_e7);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(   900000000, kAirports[i].lat_e7);
    }
}

// ---------------------------------------------------------------------------
// T07: all airport longitudes in [-180*1e7, 180*1e7]
// ---------------------------------------------------------------------------
void test_airport_lon_range() {
    for (size_t i = 0; i < kAirportCount; ++i) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(-1800000000, kAirports[i].lon_e7);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(   1800000000, kAirports[i].lon_e7);
    }
}

// ---------------------------------------------------------------------------
// T08: all runway lengths are positive
// ---------------------------------------------------------------------------
void test_runway_lengths_positive() {
    for (size_t i = 0; i < kRunwayCount; ++i) {
        TEST_ASSERT_GREATER_THAN_UINT16_MESSAGE(0, kRunways[i].length_m,
            "runway length_m must be > 0");
    }
}

// ---------------------------------------------------------------------------
// T09: all runway airport_idx values are within [0, kAirportCount)
// ---------------------------------------------------------------------------
void test_runway_index_bounds() {
    for (size_t i = 0; i < kRunwayCount; ++i) {
        TEST_ASSERT_LESS_THAN_UINT_MESSAGE(
            kAirportCount, (size_t)kRunways[i].airport_idx,
            "runway airport_idx must be in [0, kAirportCount)");
    }
}

// ---------------------------------------------------------------------------
// T10: runways are sorted by airport_idx ASC, then length_m DESC
// ---------------------------------------------------------------------------
void test_runway_ordering() {
    for (size_t i = 1; i < kRunwayCount; ++i) {
        uint16_t prevIdx = kRunways[i-1].airport_idx;
        uint16_t curIdx  = kRunways[i].airport_idx;
        TEST_ASSERT_GREATER_OR_EQUAL_UINT16_MESSAGE(prevIdx, curIdx,
            "runway airport_idx must be non-decreasing");
        if (prevIdx == curIdx) {
            TEST_ASSERT_GREATER_OR_EQUAL_UINT16_MESSAGE(
                kRunways[i].length_m, kRunways[i-1].length_m,
                "runways within same airport must be sorted by length DESC");
        }
    }
}

// ---------------------------------------------------------------------------
// T11: compile-time extent check is consistent with kAirportCount constant
// ---------------------------------------------------------------------------
void test_compile_time_extent_airport() {
    // Redundant with the static_assert, but reports at runtime too.
    TEST_ASSERT_EQUAL_UINT(kAirportCount,
                           (std::extent<decltype(kAirports)>::value));
}

// ---------------------------------------------------------------------------
// T12: compile-time extent check is consistent with kRunwayCount constant
// ---------------------------------------------------------------------------
void test_compile_time_extent_runway() {
    TEST_ASSERT_EQUAL_UINT(kRunwayCount,
                           (std::extent<decltype(kRunways)>::value));
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_airport_count_matches_constant);
    RUN_TEST(test_runway_count_matches_constant);
    RUN_TEST(test_airport_idents_are_4_chars);
    RUN_TEST(test_airport_idents_unique);
    RUN_TEST(test_airport_idents_sorted);
    RUN_TEST(test_airport_lat_range);
    RUN_TEST(test_airport_lon_range);
    RUN_TEST(test_runway_lengths_positive);
    RUN_TEST(test_runway_index_bounds);
    RUN_TEST(test_runway_ordering);
    RUN_TEST(test_compile_time_extent_airport);
    RUN_TEST(test_compile_time_extent_runway);

    return UNITY_END();
}
