#include "core/cert_time.h"

namespace core {

namespace {

bool isLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int mon) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31};
  if (mon < 1 || mon > 12) {
    return 0;
  }
  if (mon == 2 && isLeapYear(year)) {
    return 29;
  }
  return kDays[mon - 1];
}

// Days since 1970-01-01 for a valid proleptic-Gregorian y/m/d (Howard Hinnant's
// algorithm). Requires 1 <= m <= 12 and a valid day; the caller validates first.
int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;                       // [0, 399]
  const int64_t doy =
      (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (d - 1);    // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
  return era * 146097 + doe - 719468;
}

}  // namespace

bool certDateTimeToUnix(const CertDateTime& dt, int64_t* out_unix) {
  // Reject out-of-range fields as a malformed certificate date. The year range
  // is deliberately generous but finite so obviously corrupt values (0, huge)
  // are rejected rather than silently converted.
  if (dt.year < 1 || dt.year > 9999) {
    return false;
  }
  if (dt.mon < 1 || dt.mon > 12) {
    return false;
  }
  if (dt.day < 1 || dt.day > daysInMonth(dt.year, dt.mon)) {
    return false;
  }
  if (dt.hour < 0 || dt.hour > 23) {
    return false;
  }
  if (dt.min < 0 || dt.min > 59) {
    return false;
  }
  if (dt.sec < 0 || dt.sec > 59) {  // ASN.1 cert times cannot encode a leap second
    return false;
  }
  const int64_t days = daysFromCivil(dt.year, dt.mon, dt.day);
  *out_unix = days * 86400 + dt.hour * 3600 + dt.min * 60 + dt.sec;
  return true;
}

CertVerification classifyCertVerification(int64_t now_unix,
                                          const CertDateTime& not_before,
                                          const CertDateTime& not_after) {
  CertVerification result{CertValidity::Malformed, 0};
  int64_t nb_unix = 0;
  int64_t na_unix = 0;
  if (!certDateTimeToUnix(not_before, &nb_unix) ||
      !certDateTimeToUnix(not_after, &na_unix)) {
    return result;  // malformed date field: Malformed, notBefore unavailable (0)
  }
  // An inverted validity window is not a well-formed certificate period. Fail
  // closed on it explicitly rather than letting the now-relative ordering below
  // classify it as merely NotYetValid/Expired.
  if (nb_unix > na_unix) {
    return result;  // inverted window: Malformed, 0
  }
  if (now_unix < nb_unix) {
    result.validity = CertValidity::NotYetValid;
    return result;  // not yet valid: no authenticated floor candidate (0)
  }
  if (now_unix > na_unix) {
    result.validity = CertValidity::Expired;
    return result;  // expired: no authenticated floor candidate (0)
  }
  // Exactly Valid: expose the CA-signed notBefore as the authenticated lower
  // bound. This is the ONLY path that yields a non-zero not_before_unix.
  result.validity = CertValidity::Valid;
  result.not_before_unix = nb_unix;
  return result;
}

CertValidity classifyCertValidity(int64_t now_unix,
                                  const CertDateTime& not_before,
                                  const CertDateTime& not_after) {
  return classifyCertVerification(now_unix, not_before, not_after).validity;
}

bool certValidityBlocksFetch(CertValidity validity) {
  return validity != CertValidity::Valid;
}

void certChainBegin(CertChainAccumulator* acc, int64_t now_unix) {
  acc->now_unix = now_unix;
  acc->leaf_not_before_unix = 0;
  acc->count = 0;
  acc->all_valid = true;
  acc->overflow = false;
  acc->first_invalid = CertValidity::Malformed;
}

bool certChainAddNode(CertChainAccumulator* acc, const CertDateTime& not_before,
                      const CertDateTime& not_after) {
  // Refuse a node once the bound is already reached: record the overflow and tell
  // the caller to stop walking. Because a node still remained past the maximum,
  // the chain fails closed in certChainFinalize.
  if (acc->count >= kMaxPeerChainLen) {
    acc->overflow = true;
    return false;
  }
  const CertVerification node =
      classifyCertVerification(acc->now_unix, not_before, not_after);
  if (acc->count == 0) {
    // Leaf: capture its CA-signed notBefore. classifyCertVerification returns a
    // non-zero value ONLY when the leaf is exactly Valid, so a not-yet-valid /
    // expired / malformed leaf contributes 0.
    acc->leaf_not_before_unix = node.not_before_unix;
  }
  if (node.validity != CertValidity::Valid) {
    if (acc->all_valid) {
      acc->first_invalid = node.validity;  // remember the first failing node
    }
    acc->all_valid = false;
  }
  ++acc->count;
  return true;
}

CertVerification certChainFinalize(const CertChainAccumulator* acc) {
  // Empty (no peer cert) or over-long/cyclic chains are not well-formed: fail
  // closed with no authenticated floor candidate.
  if (acc->count == 0 || acc->overflow) {
    return CertVerification{CertValidity::Malformed, 0};
  }
  // Any not-yet-valid / expired / malformed node (leaf OR intermediate) blocks the
  // fetch and forces the authenticated leaf floor to 0.
  if (!acc->all_valid) {
    return CertVerification{acc->first_invalid, 0};
  }
  // Whole chain Valid: expose the leaf's CA-signed notBefore as the authenticated
  // lower bound. This is the ONLY path that yields a non-zero not_before_unix.
  return CertVerification{CertValidity::Valid, acc->leaf_not_before_unix};
}

}  // namespace core
