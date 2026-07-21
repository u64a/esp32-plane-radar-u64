#include "core/url_form.h"

#include <cstring>

namespace core {

namespace {

// Hex digit value, or -1 if not a hex digit. Accepts both cases so "%2F" and
// "%2f" decode identically.
int hexVal(uint8_t c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

enum class DecodeStatus : uint8_t { Ok, BadEncoding, TooLong };

// Percent/plus-decode one component into out (capacity out_cap, NUL-terminated on
// success). Rejects: a truncated "%H"/"%" escape, a non-hex escape, any raw
// control/DEL/non-ASCII byte, and any decoded control/NUL/DEL/high byte. The
// decoded alphabet is therefore exactly printable ASCII 0x20..0x7e -- an encoded
// byte >= 0x7f (e.g. %7f DEL, %80, the %C3%A9 bytes of UTF-8 'e-acute', %ff) is
// rejected exactly like the equivalent raw high byte, so raw and percent-encoded
// forms of the same non-printable byte are treated identically.
DecodeStatus decodeComponent(const char* src, uint16_t src_len, char* out,
                             uint16_t out_cap, uint16_t* out_len) {
  uint16_t n = 0;
  uint16_t i = 0;
  while (i < src_len) {
    uint8_t decoded;
    const uint8_t c = static_cast<uint8_t>(src[i]);
    if (c == '+') {
      decoded = ' ';
      i += 1;
    } else if (c == '%') {
      if (static_cast<uint32_t>(i) + 2U >= src_len) {
        return DecodeStatus::BadEncoding;  // need two hex digits after '%'
      }
      const int hi = hexVal(static_cast<uint8_t>(src[i + 1]));
      const int lo = hexVal(static_cast<uint8_t>(src[i + 2]));
      if (hi < 0 || lo < 0) {
        return DecodeStatus::BadEncoding;
      }
      decoded = static_cast<uint8_t>((hi << 4) | lo);
      i += 3;
    } else {
      if (c < 0x20 || c >= 0x7f) {
        return DecodeStatus::BadEncoding;  // raw control/DEL/non-ASCII
      }
      decoded = c;
      i += 1;
    }
    if (decoded < 0x20 || decoded >= 0x7f) {
      return DecodeStatus::BadEncoding;  // decoded NUL/control/DEL/high byte
    }
    if (n >= out_cap) {
      return DecodeStatus::TooLong;
    }
    out[n++] = static_cast<char>(decoded);
  }
  out[n] = '\0';
  *out_len = n;
  return DecodeStatus::Ok;
}

// Map a decoded key to its slot in the form, or nullptr if unknown.
FormField* matchKnownField(ProvisioningForm* f, const char* key, uint16_t len) {
  struct Known {
    const char* name;
    FormField ProvisioningForm::*member;
  };
  static const Known table[] = {
      {"csrf", &ProvisioningForm::csrf},
      {"ssid", &ProvisioningForm::ssid},
      {"psk", &ProvisioningForm::psk},
      {"lat", &ProvisioningForm::lat},
      {"lon", &ProvisioningForm::lon},
      {"use_miles", &ProvisioningForm::use_miles},
      {"show_runways", &ProvisioningForm::show_runways},
  };
  for (const Known& k : table) {
    if (std::strlen(k.name) == len && std::memcmp(k.name, key, len) == 0) {
      return &(f->*(k.member));
    }
  }
  return nullptr;
}

}  // namespace

FormParseResult urlFormParse(const char* body, uint16_t body_len,
                             uint16_t max_pairs, ProvisioningForm* out) {
  if (out == nullptr) {
    return FormParseResult::MalformedPair;
  }
  std::memset(out, 0, sizeof(*out));
  if (body == nullptr) {
    return FormParseResult::Ok;  // empty body: all fields absent
  }

  char key_buf[kFormFieldNameCap + 1];
  char val_buf[kFormFieldValueCap + 1];
  uint16_t pair_count = 0;
  uint16_t i = 0;
  while (i < body_len) {
    // Isolate the next "key=value" segment (up to the next '&').
    const uint16_t seg_start = i;
    while (i < body_len && body[i] != '&') {
      ++i;
    }
    const uint16_t seg_len = static_cast<uint16_t>(i - seg_start);
    const bool had_sep = (i < body_len);
    if (had_sep) {
      ++i;  // consume '&'
    }

    if (seg_len == 0) {
      return FormParseResult::MalformedPair;  // empty segment (leading/"&&")
    }
    if (++pair_count > max_pairs) {
      return FormParseResult::TooManyFields;
    }

    // Split the segment at its first '='.
    uint16_t eq = seg_start;
    while (eq < seg_start + seg_len && body[eq] != '=') {
      ++eq;
    }
    if (eq == seg_start + seg_len) {
      return FormParseResult::MalformedPair;  // no '='
    }
    const uint16_t key_len = static_cast<uint16_t>(eq - seg_start);
    const uint16_t val_len =
        static_cast<uint16_t>(seg_start + seg_len - (eq + 1));
    if (key_len == 0) {
      return FormParseResult::MalformedPair;  // empty key
    }

    uint16_t dk = 0;
    switch (decodeComponent(body + seg_start, key_len, key_buf,
                            kFormFieldNameCap, &dk)) {
      case DecodeStatus::BadEncoding:
        return FormParseResult::BadEncoding;
      case DecodeStatus::TooLong:
        return FormParseResult::FieldNameTooLong;
      case DecodeStatus::Ok:
        break;
    }
    uint16_t dv = 0;
    switch (decodeComponent(body + eq + 1, val_len, val_buf, kFormFieldValueCap,
                            &dv)) {
      case DecodeStatus::BadEncoding:
        return FormParseResult::BadEncoding;
      case DecodeStatus::TooLong:
        return FormParseResult::FieldValueTooLong;
      case DecodeStatus::Ok:
        break;
    }

    FormField* field = matchKnownField(out, key_buf, dk);
    if (field != nullptr) {
      if (field->present) {
        return FormParseResult::DuplicateField;  // duplicate known field
      }
      field->present = true;
      field->len = dv;
      std::memcpy(field->value, val_buf, dv);
      field->value[dv] = '\0';
    }
    // Unknown fields: validated and bounded above, then intentionally dropped.
  }
  return FormParseResult::Ok;
}

ProvisioningValidity validateProvisioning(const ProvisioningForm& form,
                                          uint16_t csrf_expected_len) {
  if (!form.csrf.present) {
    return ProvisioningValidity::CsrfMissing;
  }
  if (form.csrf.len != csrf_expected_len) {
    return ProvisioningValidity::CsrfInvalid;
  }
  if (!form.ssid.present) {
    return ProvisioningValidity::SsidMissing;
  }
  if (form.ssid.len == 0 || form.ssid.len > 32) {
    return ProvisioningValidity::SsidInvalid;
  }
  if (!form.psk.present) {
    return ProvisioningValidity::PskMissing;
  }
  if (form.psk.len != 0 && (form.psk.len < 8 || form.psk.len > 63)) {
    return ProvisioningValidity::PskInvalid;  // empty = open network is allowed
  }
  if (!form.lat.present) {
    return ProvisioningValidity::LatMissing;
  }
  if (form.lat.len == 0 || form.lat.len > kProvisioningCoordMaxLen) {
    return ProvisioningValidity::LatInvalid;
  }
  if (!form.lon.present) {
    return ProvisioningValidity::LonMissing;
  }
  if (form.lon.len == 0 || form.lon.len > kProvisioningCoordMaxLen) {
    return ProvisioningValidity::LonInvalid;
  }
  return ProvisioningValidity::Ok;
}

bool formCheckboxOn(const FormField& field) { return field.present; }

}  // namespace core
