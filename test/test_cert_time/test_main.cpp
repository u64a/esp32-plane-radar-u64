#include <unity.h>

#include <cstdint>

#include "core/cert_time.h"

using core::CertDateTime;
using core::CertValidity;
using core::CertVerification;

static_assert(__cplusplus >= 201703L, "Native tests require C++17");

namespace {

int v(CertValidity c) { return static_cast<int>(c); }

int64_t toUnix(const CertDateTime& dt) {
  int64_t out = -1;
  const bool ok = core::certDateTimeToUnix(dt, &out);
  TEST_ASSERT_TRUE(ok);
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- certDateTimeToUnix: known absolute epochs (proleptic Gregorian, UTC) -----

void test_epoch_known_values() {
  TEST_ASSERT_EQUAL_INT64(0, toUnix(CertDateTime{1970, 1, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT64(946684800, toUnix(CertDateTime{2000, 1, 1, 0, 0, 0}));
  TEST_ASSERT_EQUAL_INT64(1784505600, toUnix(CertDateTime{2026, 7, 20, 0, 0, 0}));
  // Leap day in a leap year converts (2024 is a leap year).
  TEST_ASSERT_EQUAL_INT64(1709164800, toUnix(CertDateTime{2024, 2, 29, 0, 0, 0}));
  // Beyond the 32-bit epoch: proves int64 arithmetic (no Y2038 wrap).
  TEST_ASSERT_EQUAL_INT64(2147483647, toUnix(CertDateTime{2038, 1, 19, 3, 14, 7}));
  TEST_ASSERT_EQUAL_INT64(2231510400, toUnix(CertDateTime{2040, 9, 17, 16, 0, 0}));
}

// --- certDateTimeToUnix: malformed fields are rejected (fail closed) ----------

void test_malformed_dates_rejected() {
  int64_t out = 12345;
  const CertDateTime bad[] = {
      {2026, 0, 1, 0, 0, 0},    // month 0
      {2026, 13, 1, 0, 0, 0},   // month 13
      {2026, 1, 0, 0, 0, 0},    // day 0
      {2026, 1, 32, 0, 0, 0},   // day 32
      {2023, 2, 29, 0, 0, 0},   // Feb 29 in a non-leap year
      {2026, 4, 31, 0, 0, 0},   // April has 30 days
      {2026, 1, 1, 24, 0, 0},   // hour 24
      {2026, 1, 1, 0, 60, 0},   // minute 60
      {2026, 1, 1, 0, 0, 60},   // second 60 (ASN.1 has no leap second)
      {2026, 1, 1, 0, 0, 61},   // second 61
      {0, 1, 1, 0, 0, 0},       // year 0
  };
  for (const auto& dt : bad) {
    out = 12345;
    TEST_ASSERT_FALSE(core::certDateTimeToUnix(dt, &out));
    TEST_ASSERT_EQUAL_INT64(12345, out);  // untouched on malformed input
  }
  // ASN.1 certificate times cannot encode a leap second: sec == 60 fails closed
  // (asserted above); sec == 59 is the last valid second and converts.
  int64_t sec = -1;
  TEST_ASSERT_TRUE(
      core::certDateTimeToUnix(CertDateTime{2026, 6, 30, 23, 59, 59}, &sec));
}

// --- classifyCertValidity: RFC 5280 inclusive [notBefore, notAfter] -----------

void test_validity_boundaries_inclusive() {
  const CertDateTime nb{2026, 1, 1, 0, 0, 0};
  const CertDateTime na{2026, 12, 31, 23, 59, 59};
  const int64_t nb_u = toUnix(nb);
  const int64_t na_u = toUnix(na);

  // before notBefore -> NotYetValid
  TEST_ASSERT_EQUAL_INT(v(CertValidity::NotYetValid),
                        v(core::classifyCertValidity(nb_u - 1, nb, na)));
  // exact notBefore -> Valid (inclusive lower bound)
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid),
                        v(core::classifyCertValidity(nb_u, nb, na)));
  // inside -> Valid
  TEST_ASSERT_EQUAL_INT(
      v(CertValidity::Valid),
      v(core::classifyCertValidity((nb_u + na_u) / 2, nb, na)));
  // exact notAfter -> Valid (inclusive upper bound)
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid),
                        v(core::classifyCertValidity(na_u, nb, na)));
  // after notAfter -> Expired
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Expired),
                        v(core::classifyCertValidity(na_u + 1, nb, na)));
}

void test_validity_malformed_window_is_malformed() {
  const CertDateTime good{2026, 1, 1, 0, 0, 0};
  const CertDateTime bad{2026, 13, 1, 0, 0, 0};  // malformed month
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed),
                        v(core::classifyCertValidity(1784505600, bad, good)));
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed),
                        v(core::classifyCertValidity(1784505600, good, bad)));
}

void test_inverted_window_is_malformed() {
  // notBefore AFTER notAfter is not a well-formed validity period: it is
  // Malformed at every now, not merely NotYetValid/Expired. This fails closed on
  // the window itself rather than relying on the now-relative ordering.
  const CertDateTime nb{2027, 1, 1, 0, 0, 0};
  const CertDateTime na{2026, 1, 1, 0, 0, 0};
  const int64_t nb_u = toUnix(nb);
  const int64_t na_u = toUnix(na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed),
                        v(core::classifyCertValidity(na_u - 1, nb, na)));
  TEST_ASSERT_EQUAL_INT(
      v(CertValidity::Malformed),
      v(core::classifyCertValidity((na_u + nb_u) / 2, nb, na)));
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed),
                        v(core::classifyCertValidity(nb_u + 1, nb, na)));
  // certValidityBlocksFetch fails closed on the inverted (Malformed) window.
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(
      core::classifyCertValidity(nb_u + 1, nb, na)));
}

// --- certValidityBlocksFetch: fail closed unless exactly Valid ----------------

void test_blocks_fetch_unless_valid() {
  TEST_ASSERT_FALSE(core::certValidityBlocksFetch(CertValidity::Valid));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(CertValidity::NotYetValid));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(CertValidity::Expired));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(CertValidity::Malformed));
}

// --- classifyCertVerification: validity + CA-signed notBefore in one pass ------

void test_verification_exposes_notbefore_only_when_valid() {
  const CertDateTime nb{2026, 1, 1, 0, 0, 0};
  const CertDateTime na{2026, 12, 31, 23, 59, 59};
  const int64_t nb_u = toUnix(nb);
  const int64_t na_u = toUnix(na);

  // Valid: the CA-signed notBefore epoch is exposed as the authenticated floor
  // candidate (exactly notBefore, never `now`).
  CertVerification at_nb = core::classifyCertVerification(nb_u, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(at_nb.validity));
  TEST_ASSERT_EQUAL_INT64(nb_u, at_nb.not_before_unix);
  CertVerification mid =
      core::classifyCertVerification((nb_u + na_u) / 2, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(mid.validity));
  TEST_ASSERT_EQUAL_INT64(nb_u, mid.not_before_unix);  // still the notBefore, not now
  CertVerification at_na = core::classifyCertVerification(na_u, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(at_na.validity));
  TEST_ASSERT_EQUAL_INT64(nb_u, at_na.not_before_unix);

  // Not yet valid: fail closed with a 0 candidate (only meaningful when Valid).
  CertVerification before = core::classifyCertVerification(nb_u - 1, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::NotYetValid), v(before.validity));
  TEST_ASSERT_EQUAL_INT64(0, before.not_before_unix);

  // Expired: fail closed with a 0 candidate.
  CertVerification after = core::classifyCertVerification(na_u + 1, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Expired), v(after.validity));
  TEST_ASSERT_EQUAL_INT64(0, after.not_before_unix);
}

void test_verification_malformed_and_inverted_yield_zero_candidate() {
  const CertDateTime good{2026, 1, 1, 0, 0, 0};
  const CertDateTime bad_month{2026, 13, 1, 0, 0, 0};  // malformed field
  CertVerification malformed =
      core::classifyCertVerification(1784505600, bad_month, good);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(malformed.validity));
  TEST_ASSERT_EQUAL_INT64(0, malformed.not_before_unix);

  // Inverted window (notBefore AFTER notAfter) is Malformed at every now, and
  // never yields an authenticated candidate.
  const CertDateTime nb{2027, 1, 1, 0, 0, 0};
  const CertDateTime na{2026, 1, 1, 0, 0, 0};
  const int64_t mid = (toUnix(nb) + toUnix(na)) / 2;
  CertVerification inverted = core::classifyCertVerification(mid, nb, na);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(inverted.validity));
  TEST_ASSERT_EQUAL_INT64(0, inverted.not_before_unix);

  // classifyCertValidity must stay in lock-step with the combined function.
  TEST_ASSERT_EQUAL_INT(v(core::classifyCertValidity(mid, nb, na)),
                        v(inverted.validity));
}

// --- Peer certificate CHAIN classification (bounded, fail-closed) -------------

namespace {

struct Span {
  CertDateTime nb;
  CertDateTime na;
};

// Drive the accumulator over spans in walk order (leaf first), breaking on a
// refused node exactly as the ESP transport does, then finalize. This is the
// pure model the ESP walk is glue for.
CertVerification classifyChain(int64_t now, const Span* spans, int n) {
  core::CertChainAccumulator acc;
  core::certChainBegin(&acc, now);
  for (int i = 0; i < n; ++i) {
    if (!core::certChainAddNode(&acc, spans[i].nb, spans[i].na)) {
      break;  // over the bound: stop walking (mirrors mbedtls_x509_crt::next)
    }
  }
  return core::certChainFinalize(&acc);
}

// A trusted "now" inside the leaf window used throughout the chain tests.
constexpr CertDateTime kNow{2026, 7, 1, 0, 0, 0};
// Leaf valid across the whole of 2026 (notBefore 2026-01-01).
constexpr Span kLeaf{{2026, 1, 1, 0, 0, 0}, {2026, 12, 31, 23, 59, 59}};

}  // namespace

void test_chain_valid_leaf_and_intermediate_passes() {
  const int64_t now = toUnix(kNow);
  const Span chain[] = {
      kLeaf,
      {{2025, 6, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}},  // valid intermediate
  };
  const CertVerification r = classifyChain(now, chain, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(r.validity));
  // Whole-chain success returns EXACTLY the leaf's notBefore (not the earlier
  // intermediate's, not now).
  TEST_ASSERT_EQUAL_INT64(toUnix(kLeaf.nb), r.not_before_unix);
}

void test_chain_expired_intermediate_fails() {
  const int64_t now = toUnix(kNow);
  const Span chain[] = {
      kLeaf,                                            // valid leaf
      {{2020, 1, 1, 0, 0, 0}, {2021, 1, 1, 0, 0, 0}},  // EXPIRED intermediate
  };
  const CertVerification r = classifyChain(now, chain, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Expired), v(r.validity));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(r.validity));
  // An invalid intermediate under a valid leaf forces the leaf floor to 0.
  TEST_ASSERT_EQUAL_INT64(0, r.not_before_unix);
}

void test_chain_future_intermediate_fails() {
  const int64_t now = toUnix(kNow);
  const Span chain[] = {
      kLeaf,                                            // valid leaf
      {{2030, 1, 1, 0, 0, 0}, {2031, 1, 1, 0, 0, 0}},  // NOT-YET-VALID intermediate
  };
  const CertVerification r = classifyChain(now, chain, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::NotYetValid), v(r.validity));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(r.validity));
  TEST_ASSERT_EQUAL_INT64(0, r.not_before_unix);
}

void test_chain_malformed_and_inverted_intermediate_fails() {
  const int64_t now = toUnix(kNow);
  // Malformed field (month 13) in the intermediate.
  const Span malformed[] = {
      kLeaf,
      {{2025, 13, 1, 0, 0, 0}, {2035, 1, 1, 0, 0, 0}},
  };
  const CertVerification rm = classifyChain(now, malformed, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(rm.validity));
  TEST_ASSERT_EQUAL_INT64(0, rm.not_before_unix);

  // Inverted window (notBefore AFTER notAfter) in the intermediate.
  const Span inverted[] = {
      kLeaf,
      {{2035, 1, 1, 0, 0, 0}, {2025, 1, 1, 0, 0, 0}},
  };
  const CertVerification ri = classifyChain(now, inverted, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(ri.validity));
  TEST_ASSERT_EQUAL_INT64(0, ri.not_before_unix);
}

void test_chain_invalid_leaf_fails() {
  const int64_t now = toUnix(kNow);
  const Span chain[] = {
      {{2020, 1, 1, 0, 0, 0}, {2021, 1, 1, 0, 0, 0}},  // EXPIRED leaf
      {{2025, 6, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}},  // valid intermediate
  };
  const CertVerification r = classifyChain(now, chain, 2);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Expired), v(r.validity));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(r.validity));
  TEST_ASSERT_EQUAL_INT64(0, r.not_before_unix);
}

void test_chain_empty_fails() {
  const int64_t now = toUnix(kNow);
  // No nodes at all (null/empty peer chain): fail closed as Malformed, 0.
  const CertVerification r = classifyChain(now, nullptr, 0);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(r.validity));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(r.validity));
  TEST_ASSERT_EQUAL_INT64(0, r.not_before_unix);
}

void test_chain_exact_max_passes() {
  const int64_t now = toUnix(kNow);
  Span chain[core::kMaxPeerChainLen];
  chain[0] = kLeaf;  // leaf carries the authenticated notBefore
  for (int i = 1; i < core::kMaxPeerChainLen; ++i) {
    chain[i] = Span{{2025, 6, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}};
  }
  const CertVerification r = classifyChain(now, chain, core::kMaxPeerChainLen);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(r.validity));
  // Exactly at the bound, every node valid: still only the leaf's notBefore.
  TEST_ASSERT_EQUAL_INT64(toUnix(kLeaf.nb), r.not_before_unix);
}

void test_chain_over_max_fails() {
  const int64_t now = toUnix(kNow);
  Span chain[core::kMaxPeerChainLen + 1];
  chain[0] = kLeaf;
  for (int i = 1; i < core::kMaxPeerChainLen + 1; ++i) {
    chain[i] = Span{{2025, 6, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}};
  }
  // One node beyond the bound (all nodes individually valid): still fails closed
  // as Malformed with no authenticated floor -- an over-long/cyclic chain is not
  // trusted.
  const CertVerification r =
      classifyChain(now, chain, core::kMaxPeerChainLen + 1);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Malformed), v(r.validity));
  TEST_ASSERT_TRUE(core::certValidityBlocksFetch(r.validity));
  TEST_ASSERT_EQUAL_INT64(0, r.not_before_unix);
}

void test_chain_returns_only_leaf_notbefore_on_success() {
  const int64_t now = toUnix(kNow);
  // Three valid nodes whose notBefore values straddle the leaf's: an earlier
  // intermediate and a later one. Whole-chain success must return the LEAF's
  // notBefore exactly -- neither the minimum, the maximum, nor now.
  const Span chain[] = {
      kLeaf,                                            // nb 2026-01-01
      {{2025, 6, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}},  // earlier notBefore
      {{2026, 3, 1, 0, 0, 0}, {2035, 6, 1, 0, 0, 0}},  // later notBefore, still valid
  };
  const CertVerification r = classifyChain(now, chain, 3);
  TEST_ASSERT_EQUAL_INT(v(CertValidity::Valid), v(r.validity));
  TEST_ASSERT_EQUAL_INT64(toUnix(kLeaf.nb), r.not_before_unix);
  TEST_ASSERT_NOT_EQUAL_INT64(now, r.not_before_unix);
  TEST_ASSERT_NOT_EQUAL_INT64(toUnix(chain[1].nb), r.not_before_unix);
  TEST_ASSERT_NOT_EQUAL_INT64(toUnix(chain[2].nb), r.not_before_unix);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_epoch_known_values);
  RUN_TEST(test_malformed_dates_rejected);
  RUN_TEST(test_validity_boundaries_inclusive);
  RUN_TEST(test_validity_malformed_window_is_malformed);
  RUN_TEST(test_inverted_window_is_malformed);
  RUN_TEST(test_blocks_fetch_unless_valid);
  RUN_TEST(test_verification_exposes_notbefore_only_when_valid);
  RUN_TEST(test_verification_malformed_and_inverted_yield_zero_candidate);
  RUN_TEST(test_chain_valid_leaf_and_intermediate_passes);
  RUN_TEST(test_chain_expired_intermediate_fails);
  RUN_TEST(test_chain_future_intermediate_fails);
  RUN_TEST(test_chain_malformed_and_inverted_intermediate_fails);
  RUN_TEST(test_chain_invalid_leaf_fails);
  RUN_TEST(test_chain_empty_fails);
  RUN_TEST(test_chain_exact_max_passes);
  RUN_TEST(test_chain_over_max_fails);
  RUN_TEST(test_chain_returns_only_leaf_notbefore_on_success);
  return UNITY_END();
}
