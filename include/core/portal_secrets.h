#pragma once

#include <cstddef>
#include <cstdint>

// Portal identity and secrets (pure, injectable entropy).
//
// Produces the SoftAP identity and the per-session secrets used by the captive
// setup portal:
//   * A stable, public AP SSID derived from the factory MAC.
//   * A 14-character WPA2 passphrase (70 bits) drawn from injected entropy using
//     a direct 5-bit-per-character mapping over a 32-symbol unambiguous alphabet
//     (no modulo, hence no bias).
//   * An independent 128-bit CSRF token (32 lowercase hex chars).
//
// Security posture (fail closed): the entropy source is injected and may fail or
// be exhausted. On any failure the output is securely zeroed and the call fails.
// There is deliberately no weak fallback -- no MAC-, time-, or counter-derived
// seed, and no reuse of a previous draw. Secrets are never logged.

namespace core {

// SoftAP SSID: "PlaneRadar-" + the last three MAC bytes as uppercase hex.
inline constexpr char kPortalSsidPrefix[] = "PlaneRadar-";
inline constexpr uint16_t kPortalSsidCap = 32;  // 802.11 SSID max (ASCII here)

// 14 characters * 5 bits = 70 bits of entropy over a 32-symbol alphabet that
// omits visually ambiguous characters (0/O, 1/I/L).
inline constexpr uint16_t kPortalPasswordLen = 14;
inline constexpr char kPortalPasswordAlphabet[] =
    "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // exactly 32 symbols

// 128-bit CSRF token rendered as 32 lowercase hex characters.
inline constexpr uint16_t kPortalCsrfTokenBytes = 16;
inline constexpr uint16_t kPortalCsrfTokenLen = 32;

// Injected entropy source. fill() must write exactly len cryptographically-strong
// random bytes and return true, or return false on failure/exhaustion (writing
// nothing meaningful). ctx is an opaque caller pointer.
using EntropyFillFn = bool (*)(void* ctx, uint8_t* out, size_t len);
struct EntropySource {
  EntropyFillFn fill;
  void* ctx;
};

// Volatile-safe zeroization for owned secret buffers. Uses volatile stores so the
// compiler cannot elide the wipe as a dead store.
void secureZero(void* buf, size_t len);

// Format the SoftAP SSID into out (needs >= 18 bytes). Returns false if out_cap
// is too small. The result is always <= 32 ASCII characters.
bool formatPortalSsid(const uint8_t mac[6], char* out, size_t out_cap);

// Generate the 14-character WPA2 passphrase into out (needs >= 15 bytes). On
// entropy failure, out is zeroed and false is returned.
bool generatePortalPassword(const EntropySource& entropy, char* out,
                            size_t out_cap);

// Generate the 32-hex-character CSRF token into out (needs >= 33 bytes). On
// entropy failure, out is zeroed and false is returned.
bool generateCsrfToken(const EntropySource& entropy, char* out, size_t out_cap);

// Constant-time, full-length comparison of two csrf tokens. Always inspects all
// len bytes regardless of where a mismatch occurs, so timing does not leak the
// match prefix. The caller must ensure both operands hold len bytes (length is
// not secret and is validated separately). A zero length returns false: an empty
// token can never authenticate (fail closed).
bool csrfTokenEqual(const char* a, const char* b, size_t len);

// The complete per-session portal identity + secrets. Bound to a session so the
// CSRF comparison is always against the token issued for that session.
struct PortalSecrets {
  char ssid[kPortalSsidCap + 1];             // public AP SSID
  char password[kPortalPasswordLen + 1];     // WPA2 passphrase (secret)
  char csrf[kPortalCsrfTokenLen + 1];        // CSRF token (secret)
  bool valid;                                // true iff all fields were generated
};

// Generate a fresh identity + secrets bundle. Draws password and CSRF entropy
// with two independent fill() calls (no reuse). If any step fails, the entire
// bundle is zeroed, valid is set false, and false is returned.
bool generatePortalSecrets(const uint8_t mac[6], const EntropySource& entropy,
                           PortalSecrets* out);

// Securely wipe a secrets bundle and mark it invalid.
void zeroPortalSecrets(PortalSecrets* secrets);

}  // namespace core
