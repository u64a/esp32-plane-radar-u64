#pragma once

// Thin ESP32 WiFiClientSecure adapter implementing the Arduino-free transport
// seams. TLS policy is intentionally isolated here so Phase 7 can add CA/SNTP
// verification without touching the HTTP decoder or parser. This header is
// ESP-only and must never be compiled into the native shared build.

#include <WiFiClientSecure.h>

#include <cstdint>

#include "core/cert_time.h"
#include "services/adsb_transport.h"

namespace services::adsb {

// Non-blocking byte source over an established TLS connection.
class EspTlsByteSource : public ByteSource {
 public:
  explicit EspTlsByteSource(WiFiClientSecure& client) : client_(client) {}
  ReadStatus read(uint8_t* buffer, size_t capacity, size_t* out_len) override;

 private:
  WiFiClientSecure& client_;
};

class EspMillisClock : public Clock {
 public:
  uint32_t nowMs() const override;
};

// Resolve DNS (distinguishing DnsFailure), then perform the TCP + TLS connect
// against the resolved IP using the SNI-capable IP+host overload (no second DNS
// lookup). The connect budget (DNS + TCP + TLS) is cumulative: it starts before
// DNS, and only the time left after DNS bounds the socket + handshake. Because
// connect(IP,port,host,...) applies setTimeout() to the TCP select and a
// SEPARATE setHandshakeTimeout() to the handshake, that remainder is partitioned
// into non-overlapping whole-second TCP and TLS slices (favoring TLS) whose sum
// stays within the budget -- never the full remainder to both. If fewer than two
// whole seconds remain the split is not viable and the result is Timeout without
// starting connect(). A late success (connect() returns after the absolute
// budget is spent) is rejected: the socket is stopped and Timeout returned. A
// failure that consumes the whole budget is Timeout; an earlier failure is
// TlsFailure. Exception: WiFi.hostByName() has no timeout knob, so DNS may run up
// to the ESP-IDF resolver's ~15 s core timeout that connect_timeout_ms cannot
// preempt. This connect call blocks; the poll callback cannot run inside it.
// TLS trust (Phase 7): pass the pinned CA bundle (services/adsb_ca_bundle.h) as
// ca_bundle. Because it is non-null and setInsecure() is NEVER called, mbedTLS
// runs with MBEDTLS_SSL_VERIFY_REQUIRED and verifies BOTH the CA chain and the
// hostname (host drives SNI and CN/SAN verification). There is no insecure
// fallback: a chain/hostname failure returns TlsFailure and the body is never
// sent. Callers MUST additionally call espVerifyPeerCertValidity() before
// sending, because the pinned SDK disables mbedTLS notBefore/notAfter checks.
ConnectOutcome espTlsConnect(WiFiClientSecure& client, const char* host,
                             uint16_t port, uint32_t connect_timeout_ms,
                             const char* ca_bundle);

// After a verified handshake, walk the FULL retained peer certificate chain (leaf
// plus every peer-supplied intermediate / cross-cert, via mbedtls_x509_crt::next)
// and classify every node's validity at now_unix (trusted UTC epoch seconds),
// returning the whole-chain validity AND the CA-signed leaf notBefore epoch
// (core::CertVerification). This performs the explicit notBefore/notAfter
// enforcement that the pinned mbedTLS build omits for EVERY node
// (CONFIG_MBEDTLS_HAVE_TIME_DATE is disabled), not just the leaf. A missing/empty
// chain, any not-yet-valid / expired / malformed (or inverted-window) node, or a
// chain longer than core::kMaxPeerChainLen is treated as {Malformed, ...} so the
// caller fails closed. The head pointer is fetched once and the walk makes no
// further SSL call and no allocation. not_before_unix is meaningful only when
// validity == Valid, i.e. only when the WHOLE chain is Valid (0 for any invalid
// node, including an invalid intermediate under a valid leaf), so the caller may
// feed it straight into the authenticated persisted-floor ratchet without further
// trust checks -- an invalid intermediate forces a 0 floor candidate. The caller
// must reject the connection (stop + fail) whenever core::certValidityBlocksFetch()
// is true, BEFORE sending any HTTP request. NOTE: getPeerCertificate() does not
// expose the locally pinned trust anchor that actually terminated verification,
// so this enforces the dates of what the PEER supplied; the pinned root's own
// identity/validity is guaranteed offline by scripts/verify-ca-bundle.ps1.
core::CertVerification espVerifyPeerCertValidity(WiFiClientSecure& client,
                                                 int64_t now_unix);

// Send the whole request, tolerating partial writes and yielding on back-pressure.
bool espSendAll(WiFiClientSecure& client, const uint8_t* data, size_t length,
                Clock& clock, IdleHandler& idle, uint32_t deadline_ms);

}  // namespace services::adsb
