#pragma once

#include <cstdint>

namespace core {

// Broken-down UTC calendar date/time as carried by an X.509 certificate's
// notBefore / notAfter fields. This mirrors the integer layout of
// mbedtls_x509_time (year, mon, day, hour, min, sec; UTC; no timezone). The ESP
// transport copies mbedtls_x509_time into this struct so every date computation
// stays Arduino-free and unit-testable.
struct CertDateTime {
  int year;  // full year, e.g. 2026
  int mon;   // 1..12
  int day;   // 1..31 (validated against the month/leap year)
  int hour;  // 0..23
  int min;   // 0..59
  int sec;   // 0..59 (ASN.1 UTCTime/GeneralizedTime seconds; 60 is NOT allowed)
};

// Convert a broken-down UTC date/time to Unix epoch seconds using the proleptic
// Gregorian calendar (no leap seconds, no timezone). Returns false and leaves
// *out_unix untouched when any field is out of range -- i.e. a malformed
// certificate date -- so callers fail closed. Seconds outside 0..59 are rejected
// (ASN.1 certificate times cannot encode a leap second). Deterministic and
// independent of the C library's timezone-sensitive mktime/timegm.
bool certDateTimeToUnix(const CertDateTime& dt, int64_t* out_unix);

// Result of comparing trusted UTC time against a certificate's validity window.
enum class CertValidity : uint8_t {
  Valid,        // notBefore <= now <= notAfter (RFC 5280 inclusive bounds)
  NotYetValid,  // now < notBefore
  Expired,      // now > notAfter
  Malformed,    // notBefore/notAfter out-of-range, or notBefore > notAfter (fail closed)
};

// A certificate's validity AND its CA-signed notBefore epoch, computed together.
// not_before_unix is the authenticated issuance lower bound (proof the CA vouched
// the certificate did not exist before it) and is meaningful ONLY when validity
// == Valid; for every fail-closed classification (NotYetValid / Expired /
// Malformed / a null cert at the call site) it is forced to 0 so an unverified
// or not-yet-valid certificate can never contribute a persisted-floor candidate.
struct CertVerification {
  CertValidity validity;
  int64_t not_before_unix;  // CA-signed notBefore epoch; 0 unless validity == Valid
};

// Classify a certificate's validity at now_unix AND extract its CA-signed
// notBefore epoch in one pass (RFC 5280 semantics: the validity period runs from
// notBefore THROUGH notAfter, inclusive). A malformed notBefore/notAfter or an
// INVERTED window (notBefore > notAfter) yields {Malformed, 0}, failing closed on
// the well-formedness of the window rather than the now-relative ordering. The
// returned not_before_unix is non-zero only when the result is exactly Valid, so
// it is safe to feed straight into the authenticated persisted-floor ratchet.
CertVerification classifyCertVerification(int64_t now_unix,
                                          const CertDateTime& not_before,
                                          const CertDateTime& not_after);

// Classify a certificate's validity at now_unix using RFC 5280 semantics: the
// validity period runs from notBefore THROUGH notAfter, inclusive. A malformed
// notBefore or notAfter yields Malformed. An INVERTED window (notBefore >
// notAfter) is treated as Malformed explicitly -- it fails closed on the well-
// formedness of the window rather than relying on the now-relative ordering
// classifications. Thin wrapper over classifyCertVerification (same decision).
CertValidity classifyCertValidity(int64_t now_unix,
                                  const CertDateTime& not_before,
                                  const CertDateTime& not_after);

// True unless the certificate is exactly Valid. The ESP transport uses this to
// fail closed (a missing, not-yet-valid, expired, or malformed peer certificate)
// before sending any HTTP request over the connection.
bool certValidityBlocksFetch(CertValidity validity);

// --- Peer certificate CHAIN classification (fail-closed, bounded) ------------

// Maximum number of peer-supplied certificates traversed in the retained chain
// (leaf + any intermediates / cross-signed certs). A conservative FIXED bound
// that stops an unbounded or cyclic mbedtls_x509_crt::next walk: it is far above
// any real chain the pinned CAs present (a leaf plus a couple of intermediates).
// A chain with MORE nodes than this fails closed (Malformed) rather than being
// partially trusted -- see certChainFinalize.
constexpr int kMaxPeerChainLen = 8;

// Running state for classifying a peer certificate chain node-by-node with NO
// allocation and no back-reference to the certificates themselves. The ESP
// transport copies each mbedtls_x509_crt node's UTC validity fields into a
// CertDateTime and feeds them in walk order (leaf first), so the whole trust
// decision stays Arduino-free and unit-testable off hardware. Treat the fields as
// opaque; drive it only through certChainBegin / certChainAddNode /
// certChainFinalize.
struct CertChainAccumulator {
  int64_t now_unix;              // trusted UTC the WHOLE chain is checked against
  int64_t leaf_not_before_unix;  // node-0 CA-signed notBefore (only when node 0 Valid)
  int count;                     // nodes accepted so far (0..kMaxPeerChainLen)
  bool all_valid;                // every accepted node classified exactly Valid
  bool overflow;                 // a node beyond kMaxPeerChainLen was offered (fail closed)
  CertValidity first_invalid;    // classification of the FIRST non-Valid node
};

// Begin a fresh chain classification at now_unix (trusted UTC epoch seconds).
// Resets the accumulator to an empty, still-passing state.
void certChainBegin(CertChainAccumulator* acc, int64_t now_unix);

// Offer the NEXT chain node in walk order (leaf first, then each ::next). Each
// node's window is classified with the SAME now_unix via classifyCertVerification
// (RFC 5280 inclusive bounds; inverted/out-of-range windows are Malformed).
// Returns true if the node was accepted (there was room and it was classified);
// returns false WITHOUT classifying it once kMaxPeerChainLen nodes have already
// been accepted -- in that case it records an overflow and the caller MUST stop
// walking (another node still remained past the bound). A NotYetValid / Expired /
// Malformed node marks the chain invalid but is still counted; only the leaf
// (node 0) contributes the authenticated notBefore, and only if it is Valid.
bool certChainAddNode(CertChainAccumulator* acc, const CertDateTime& not_before,
                      const CertDateTime& not_after);

// Collapse the accumulated chain into a single fail-closed verdict:
//   * empty chain (count == 0)          -> {Malformed, 0}  (no peer cert)
//   * overflow (more than max nodes)    -> {Malformed, 0}  (over-long / cyclic)
//   * any node not exactly Valid        -> {first_invalid, 0}
//   * EVERY node Valid (1..max nodes)   -> {Valid, leaf_not_before_unix}
// The authenticated leaf notBefore is exposed ONLY on whole-chain success, so an
// expired / not-yet-valid / malformed intermediate forces the persisted-floor
// candidate to 0 and can never ratchet NVS. Pure and Arduino-free.
CertVerification certChainFinalize(const CertChainAccumulator* acc);

}  // namespace core
