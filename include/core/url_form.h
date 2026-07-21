#pragma once

#include <cstdint>

// application/x-www-form-urlencoded decoder and provisioning-form validator.
//
// Consumes the raw request body produced by the HTTP parser and extracts only a
// fixed, known set of fields into caller-owned fixed buffers. Everything is fail
// closed and allocation-free:
//   * "%HH" and "+" are decoded; a malformed escape or a decoded NUL/control/DEL
//     byte is rejected outright (no lossy substitution).
//   * Only known fields are stored. Unknown fields are still fully decoded and
//     bounds-checked (so they cannot smuggle oversized/!malformed data) but are
//     then discarded. A duplicate of a KNOWN field is rejected.
//   * Decoded values are retained solely so the caller can act on them; they are
//     never logged or reflected back to the client.

namespace core {

// Fixed capacities. The value cap sits comfortably above the largest legal field
// (a 63-byte WPA2 passphrase) so that a merely over-long value is caught by
// semantic validation rather than the decoder, while absurd values still hit the
// decoder's hard cap.
inline constexpr uint16_t kFormFieldNameCap = 32;
inline constexpr uint16_t kFormFieldValueCap = 96;

// One decoded field. `value` is NUL-terminated and holds `len` decoded bytes.
struct FormField {
  bool present;
  uint16_t len;
  char value[kFormFieldValueCap + 1];
};

// The complete recognized provisioning form. Unknown fields are not represented.
struct ProvisioningForm {
  FormField csrf;
  FormField ssid;
  FormField psk;
  FormField lat;
  FormField lon;
  FormField use_miles;
  FormField show_runways;
};

// Result of decoding the body into a ProvisioningForm.
enum class FormParseResult : uint8_t {
  Ok = 0,
  BadEncoding,        // malformed percent-escape, or a decoded/raw control byte
  FieldNameTooLong,   // a key exceeded kFormFieldNameCap
  FieldValueTooLong,  // a value exceeded kFormFieldValueCap
  TooManyFields,      // pair count exceeded max_pairs
  DuplicateField,     // a known field appeared more than once
  MalformedPair,      // empty segment, empty key, or a segment with no '='
};

// Decode `body` (body_len bytes) into *out. At most max_pairs key=value segments
// are processed before TooManyFields. *out is fully reset first, so absent fields
// report present == false.
FormParseResult urlFormParse(const char* body, uint16_t body_len,
                             uint16_t max_pairs, ProvisioningForm* out);

// Semantic validity of the decoded provisioning fields, independent of decoding.
enum class ProvisioningValidity : uint8_t {
  Ok = 0,
  CsrfMissing,
  CsrfInvalid,  // length != expected
  SsidMissing,
  SsidInvalid,  // empty or longer than 32 bytes
  PskMissing,
  PskInvalid,  // present but length is in (0, 8) or greater than 63
  LatMissing,
  LatInvalid,  // empty or longer than the coordinate string bound
  LonMissing,
  LonInvalid,
};

// Longest accepted coordinate string (numeric range is validated separately by
// core::parseCoordinates; this only bounds the raw text).
inline constexpr uint16_t kProvisioningCoordMaxLen = 20;

// Validate the required provisioning fields. csrf is checked first because it is
// the anti-forgery gate; the remaining fields follow. use_miles/show_runways are
// optional (their absence means "off") and are not validated here.
ProvisioningValidity validateProvisioning(const ProvisioningForm& form,
                                          uint16_t csrf_expected_len);

// Interpret a checkbox-style field: present means checked/on. The concrete value
// is irrelevant (the portal HTML emits the field only when checked), and it is
// never reflected.
bool formCheckboxOn(const FormField& field);

}  // namespace core
