#pragma once

// Thin ESP32 adapter around the Arduino-free trusted-UTC core (core/time_trust.h).
// It owns the SNTP wiring, the thread-safe sample latch, the persisted-floor NVS
// storage, and the single core::TimeTrustState; every trust/rejection/floor
// DECISION lives in the core so it stays unit-testable without hardware. This
// header is ESP-only and must never be compiled into the native shared build.

#include <cstdint>

namespace services::timekeeper {

// One-time boot init: read + validate the versioned persisted-floor record from
// NVS (ignoring corrupt/partial/out-of-range blobs without weakening the release
// floor), seed the core state machine UNTRUSTED (every boot must re-sync), disable
// DHCP-provided NTP, and register the minimal SNTP sync-notification callback.
// Non-blocking; does not start SNTP (that happens on the first Wi-Fi-up tick).
void init();

// Advance the non-blocking trust state machine one tick with the current link
// state and millis(). Consumes any latched SNTP sample, validates it in the
// core, and arms/re-arms SNTP via the pinned non-blocking configTime path when
// the core asks. NEVER calls blocking getLocalTime(). Call every main loop.
void update(bool wifi_connected, uint32_t now_ms);

// True once a fresh SNTP sample has been accepted THIS boot (core Trusted phase).
// ADS-B connections are forbidden until this returns true.
bool trusted();

// Current trusted UTC epoch seconds from the DERIVED accepted-sample clock: the
// last accepted SNTP sample plus the rollover-safe monotonic millis() elapsed
// since it was accepted -- NEVER the mutable system wall clock (::time), which
// lwIP mutates via settimeofday before the sync callback even for a sample the
// core later rejects. Returns 0 when not trusted OR when the accepted sample is
// older than the trusted-sample max age (stale), so callers fail closed.
int64_t nowUnix();

// After a COMPLETE, CA+hostname+date verified ADS-B response, offer to ratchet
// the persisted floor forward using the CA-signed peer leaf certificate's
// notBefore epoch (authenticated_cert_not_before_unix from the FetchResult). The
// candidate is NEVER an SNTP-derived timestamp -- an unauthenticated NTP attacker
// cannot choose a CA-signed notBefore -- so this method reads no trusted/derived
// clock at all. Pass 0 for any non-Ok path (it is a no-op). The core persists the
// EXACT authenticated notBefore only when it advances the stored floor by at
// least 24 h and is within [release, release + ceiling]; a same/older/alternate
// cert neither writes nor rolls back. An NVS write failure is surfaced (payload-
// free log) but never weakens trust, crashes, or blocks networking.
void noteVerifiedCertFloor(int64_t authenticated_cert_not_before_unix,
                           uint32_t now_ms);

// Clear the dedicated persisted time-floor NVS namespace so a user can recover
// from corrupted/poisoned trust metadata via the credential/factory-reset path.
// Returns true on success (including when nothing was stored). Returns false if
// the namespace cannot be opened or cleared, so the caller never claims a
// successful reset when the floor record may survive.
bool clearPersistedFloor();

}  // namespace services::timekeeper
