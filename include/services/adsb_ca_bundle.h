#pragma once

// Explicit, minimal TLS trust anchor bundle for the ADS-B HTTPS endpoint
// (opendata.adsb.fi, fronted by Cloudflare). This is the ONLY set of trust
// anchors the firmware accepts for that endpoint; there is no insecure fallback,
// no fingerprint-only path, and no trust-on-first-use.
//
// Contents (official, self-signed roots only) cover the GTS and Let's Encrypt
// RSA/ECDSA chains CURRENTLY OBSERVED for this Cloudflare-fronted endpoint; this
// set is NOT claimed to cover every possible future Cloudflare CA rotation. An
// unexpected rotation to a root outside this set fails CLOSED -- the handshake
// returns TlsFailure and no request is sent -- and requires a bundle update:
//   * Let's Encrypt / ISRG  : ISRG Root X1 (RSA), ISRG Root X2 (ECDSA)
//   * Google Trust Services : GTS Root R1 (RSA),  GTS Root R4 (ECDSA)
//
// The exact certificate set, SHA-256 fingerprints, subjects/issuers, and
// validity windows are pinned and re-verified offline by
// scripts/verify-ca-bundle.ps1 (which also proves the production source never
// calls setInsecure() and always passes this bundle through the IP+host connect
// overload). Update provenance/procedure are documented in the README.
//
// The bundle is stored once in flash/rodata (a single const array). Callers must
// pass kAdsbCaBundle straight into WiFiClientSecure::connect(ip, port, host, CA,
// ...) so mbedTLS performs full CA-chain and hostname verification. NOTE: the
// pinned ESP32-C3 SDK builds mbedTLS WITHOUT CONFIG_MBEDTLS_HAVE_TIME_DATE, so
// the handshake does NOT enforce notBefore/notAfter for ANY node -- the
// application performs an explicit, fail-closed validity check that walks the
// full PEER-SUPPLIED chain (leaf + intermediates/cross-certs) against trusted UTC
// after the handshake (see services/adsb_transport_esp.h and core/cert_time.h).
// getPeerCertificate() does NOT expose the locally pinned root that terminated
// verification, so that runtime check enforces the dates of what the peer sent;
// the pinned roots' own identity/validity is maintained by the deterministic
// OFFLINE gate (scripts/verify-ca-bundle.ps1), not by runtime root-date checks.

#include <cstddef>

namespace services::adsb {

// NUL-terminated PEM bundle of the four pinned self-signed roots, concatenated.
// Defined once in adsb_ca_bundle.cpp so there is exactly one copy in flash.
extern const char kAdsbCaBundle[];

// Number of certificates embedded in kAdsbCaBundle. The offline gate asserts the
// committed bundle contains exactly this many BEGIN CERTIFICATE blocks.
extern const size_t kAdsbCaBundleCertCount;

}  // namespace services::adsb
