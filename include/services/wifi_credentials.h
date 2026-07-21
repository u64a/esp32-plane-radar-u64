#pragma once

// Fixed-size STA credential primitives for the secure provisioning transaction
// (ESP-only). Everything works on a caller-owned wifi_config_t (no Arduino
// String, no heap) so the adapter can keep the working and candidate credentials
// in static RAM, compare and zeroize them deterministically, and control exactly
// when a credential touches flash via explicit esp_wifi_set_storage() calls.
//
// The password bytes live only inside these fixed structs; this module never logs
// them and never copies them into any growable buffer.

#include <cstddef>
#include <cstdint>

#include <esp_wifi_types.h>  // wifi_config_t

namespace services::wifi_creds {

// Longest legal field lengths (802.11 SSID + WPA2 passphrase).
inline constexpr size_t kSsidMax = 32;
inline constexpr size_t kPskMax = 63;

// Snapshot the driver's current STA config into *out. Requires the WiFi driver to
// be started (WIFI_STA mode). Returns false on error.
bool snapshotSta(wifi_config_t* out);

// Build a fixed STA wifi_config_t from raw (non-NUL-terminated) byte spans,
// mirroring the Arduino WiFiSTA config so a candidate connects identically:
// WIFI_ALL_CHANNEL_SCAN, sort-by-signal, PMF capable, rssi floor -127. A
// psk_len == 0 builds an OPEN-network candidate (threshold WIFI_AUTH_OPEN);
// otherwise threshold is WIFI_AUTH_WPA2_PSK. Returns false if a span exceeds its
// field capacity (ssid > 32 or psk > 63); *out is zeroed first regardless.
bool buildSta(wifi_config_t* out, const char* ssid, size_t ssid_len,
              const char* psk, size_t psk_len);

// Build a fully-populated SoftAP wifi_config_t for the secure setup portal, so
// the very first beacon already carries the session secret: WPA2-PSK authmode,
// pairwise CCMP, explicit ssid_len/channel/max_connection and hidden flag. The
// passphrase MUST be a valid WPA2 length (8..63); a shorter psk (which would make
// the AP silently OPEN) is rejected. Returns false if ssid is empty/oversized,
// psk is out of range, or out is null; *out is zeroed first regardless.
bool buildAp(wifi_config_t* out, const char* ssid, size_t ssid_len,
             const char* psk, size_t psk_len, uint8_t channel, uint8_t max_conn,
             bool hidden);

// True iff two configs carry byte-identical credentials (SSID + password only;
// scan/threshold/pmf fields are ignored). Full-length compare over the fixed
// fields.
bool sameCredentials(const wifi_config_t& a, const wifi_config_t& b);

// True iff the config carries a non-empty SSID.
bool hasSsid(const wifi_config_t& c);

// Copy the SSID text (NUL-terminated) into out. Never touches the password.
void copySsid(const wifi_config_t& c, char* out, size_t out_cap);

// True iff the config's SSID equals the [ssid, ssid_len) span exactly.
bool ssidEquals(const wifi_config_t& c, const char* ssid, size_t ssid_len);

// Copy only the password field from src into *dst (used for the blank-PSK reuse
// case: keep the submitted SSID but inherit the stored password without ever
// exposing it). Also copies the threshold authmode so the reused password keeps
// its security class.
void inheritPassword(const wifi_config_t& src, wifi_config_t* dst);

// Volatile-safe wipe of the whole struct (uses core::secureZero semantics).
void zeroize(wifi_config_t* c);

// Explicit storage-domain selection for the following set/get/erase call.
bool setStorageRam();
bool setStorageFlash();

// Apply / read-back / erase the STA config through the driver. apply() writes to
// whichever storage domain was last selected; getSta() reads it back; eraseSta()
// persists an empty STA credential to FLASH, verifies the read-back is empty, and
// always returns storage selection to RAM (the only erase-without-replacement).
// Returns true only when the flash erase read back empty.
bool applySta(const wifi_config_t& c);
bool getSta(wifi_config_t* out);
bool eraseSta();

}  // namespace services::wifi_creds
