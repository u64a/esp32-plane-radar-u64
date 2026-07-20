#include "services/adsb_stream_parser.h"

#include <cmath>
#include <cstring>

#include "services/adsb_object_decoder.h"

namespace services::adsb {

namespace {

bool isWhitespace(uint8_t b) {
  return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

bool isDigit(uint8_t b) { return b >= '0' && b <= '9'; }

int hexDigit(uint8_t b) {
  if (b >= '0' && b <= '9') return b - '0';
  if (b >= 'a' && b <= 'f') return b - 'a' + 10;
  if (b >= 'A' && b <= 'F') return b - 'A' + 10;
  return -1;
}

uint32_t unescapeSimple(uint8_t b) {
  switch (b) {
    case 'b':
      return 0x08;
    case 'f':
      return 0x0C;
    case 'n':
      return 0x0A;
    case 'r':
      return 0x0D;
    case 't':
      return 0x09;
    default:
      return b;  // '"', '\\', '/'
  }
}

// Great-circle distance in kilometres. Correct globally, unlike the local radar
// projection whose out-of-range sentinel must not rank far, untrusted targets.
float haversineKm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double kDegToRad = 0.017453292519943295;
  constexpr double kEarthRadiusKm = 6371.0088;
  const double phi1 = lat1 * kDegToRad;
  const double phi2 = lat2 * kDegToRad;
  const double dphi = (lat2 - lat1) * kDegToRad;
  const double dlambda = (lon2 - lon1) * kDegToRad;
  const double sin_dphi = std::sin(dphi * 0.5);
  const double sin_dlambda = std::sin(dlambda * 0.5);
  double a = sin_dphi * sin_dphi +
             std::cos(phi1) * std::cos(phi2) * sin_dlambda * sin_dlambda;
  if (a < 0.0) a = 0.0;
  if (a > 1.0) a = 1.0;
  const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return static_cast<float>(kEarthRadiusKm * c);
}

}  // namespace

StreamParser::StreamParser(double center_lat, double center_lon,
                           const ParseOptions& options,
                           const ParserWorkspace& workspace)
    : center_lat_(center_lat),
      center_lon_(center_lon),
      limits_(options.limits),
      show_ground_(options.show_ground),
      ws_(workspace),
      state_(State::Root),
      str_state_(StrState::Normal),
      num_state_(NumState::IntDigits),
      error_(ParseError::None),
      depth_(0),
      ac_seen_(false),
      pending_key_is_ac_(false),
      ac_array_depth_(0),
      str_is_key_(false),
      emit_key_(false),
      key_len_(0),
      key_overflow_(false),
      hex_count_(0),
      hex_val_(0),
      pending_high_(0),
      utf8_remaining_(0),
      utf8_lo_(0),
      utf8_hi_(0),
      lit_target_(nullptr),
      lit_index_(0),
      capturing_(false),
      capture_len_(0),
      capture_base_depth_(0),
      ac_entry_count_(0),
      current_element_ordinal_(0),
      count_(0) {
  if (limits_.max_depth > kParserDepthCap) {
    limits_.max_depth = kParserDepthCap;
  }
  if (limits_.max_aircraft > kMaxAircraft) {
    limits_.max_aircraft = kMaxAircraft;
  }
}

void StreamParser::fail(ParseError err) {
  if (error_ == ParseError::None) {
    error_ = err;
  }
}

bool StreamParser::appendCapture(uint8_t byte) {
  if (capture_len_ >= limits_.max_object_bytes ||
      capture_len_ + 1 >= ws_.object_capacity) {
    fail(ParseError::TooLarge);
    return false;
  }
  ws_.object_buffer[capture_len_++] = static_cast<char>(byte);
  return true;
}

bool StreamParser::currentFrameIsArrayAc() const {
  return depth_ > 0 && frame_is_object_[depth_ - 1] == 0 &&
         frame_is_ac_[depth_ - 1];
}

bool StreamParser::pushContainer(bool is_object, bool is_ac) {
  if (depth_ >= limits_.max_depth) {
    fail(ParseError::TooLarge);
    return false;
  }
  frame_is_object_[depth_] = is_object ? 1 : 0;
  frame_is_ac_[depth_] = is_ac;
  ++depth_;
  state_ = is_object ? State::ObjKeyOrClose : State::ArrValueOrClose;
  return true;
}

void StreamParser::closeContainer(bool expect_object) {
  if (depth_ == 0) {
    fail(ParseError::Grammar);
    return;
  }
  const bool top_is_object = frame_is_object_[depth_ - 1] != 0;
  if (top_is_object != expect_object) {
    fail(ParseError::Grammar);
    return;
  }
  --depth_;
  state_ = depth_ == 0 ? State::EndDoc : State::AfterValue;
}

void StreamParser::startKeyString() {
  state_ = State::Str;
  str_state_ = StrState::Normal;
  str_is_key_ = true;
  emit_key_ = depth_ == 1;  // only the root object owns the semantic "ac" key
  key_len_ = 0;
  key_overflow_ = false;
}

void StreamParser::startValueString() {
  state_ = State::Str;
  str_state_ = StrState::Normal;
  str_is_key_ = false;
  emit_key_ = false;
}

void StreamParser::appendKeyChar(uint32_t codepoint) {
  if (codepoint > 0x7F || key_len_ >= sizeof(key_buf_)) {
    key_overflow_ = true;
    return;
  }
  key_buf_[key_len_++] = static_cast<char>(codepoint);
}

void StreamParser::dispatchValue(uint8_t byte) {
  const bool ac_value = pending_key_is_ac_;
  pending_key_is_ac_ = false;
  if (ac_value && byte != '[') {
    fail(ParseError::Grammar);  // a non-array "ac" value is fatal
    return;
  }

  switch (byte) {
    case '{': {
      if (currentFrameIsArrayAc()) {
        if (depth_ >= limits_.max_depth) {
          fail(ParseError::TooLarge);
          return;
        }
        capture_base_depth_ = depth_;
        capture_len_ = 0;
        if (!appendCapture('{')) {
          return;
        }
        capturing_ = true;
        pushContainer(true, false);
      } else {
        pushContainer(true, false);
      }
      break;
    }
    case '[': {
      const bool is_ac = ac_value;
      if (!pushContainer(false, is_ac)) {
        return;
      }
      if (is_ac) {
        ac_seen_ = true;
        ac_array_depth_ = depth_;
      }
      break;
    }
    case '"':
      startValueString();
      break;
    case 't':
      state_ = State::Lit;
      lit_target_ = "true";
      lit_index_ = 1;
      break;
    case 'f':
      state_ = State::Lit;
      lit_target_ = "false";
      lit_index_ = 1;
      break;
    case 'n':
      state_ = State::Lit;
      lit_target_ = "null";
      lit_index_ = 1;
      break;
    default:
      if (byte == '-' || isDigit(byte)) {
        state_ = State::Num;
        if (byte == '-') {
          num_state_ = NumState::AfterMinus;
        } else if (byte == '0') {
          num_state_ = NumState::LeadZero;
        } else {
          num_state_ = NumState::IntDigits;
        }
      } else {
        fail(ParseError::Grammar);
      }
      break;
  }
}

void StreamParser::closeString() {
  if (str_is_key_) {
    state_ = State::Colon;
    if (emit_key_) {
      const bool is_ac = !key_overflow_ && key_len_ == 2 && key_buf_[0] == 'a' &&
                         key_buf_[1] == 'c';
      if (is_ac) {
        if (ac_seen_) {
          fail(ParseError::Grammar);  // duplicate semantic "ac" key
          return;
        }
        pending_key_is_ac_ = true;
      } else {
        pending_key_is_ac_ = false;
      }
    }
  } else {
    state_ = State::AfterValue;
  }
}

// Classify a UTF-8 lead byte using the Unicode standard's Table 3-7 ranges. The
// range required of the FIRST continuation byte encodes the overlong, surrogate,
// and > U+10FFFF constraints; all later continuation bytes are 0x80..0xBF.
// Returns false for any byte that can never begin a valid sequence.
bool StreamParser::beginUtf8(uint8_t lead) {
  if (lead < 0xC2) {
    return false;  // 0x80..0xBF lone continuation, or 0xC0/0xC1 overlong
  }
  if (lead < 0xE0) {  // C2..DF: 2-byte sequence
    utf8_remaining_ = 1;
    utf8_lo_ = 0x80;
    utf8_hi_ = 0xBF;
  } else if (lead < 0xF0) {  // E0..EF: 3-byte sequence
    utf8_remaining_ = 2;
    if (lead == 0xE0) {
      utf8_lo_ = 0xA0;  // reject overlong E0 80..9F
      utf8_hi_ = 0xBF;
    } else if (lead == 0xED) {
      utf8_lo_ = 0x80;
      utf8_hi_ = 0x9F;  // reject UTF-8 encoded surrogates ED A0..BF
    } else {
      utf8_lo_ = 0x80;
      utf8_hi_ = 0xBF;
    }
  } else if (lead <= 0xF4) {  // F0..F4: 4-byte sequence
    utf8_remaining_ = 3;
    if (lead == 0xF0) {
      utf8_lo_ = 0x90;  // reject overlong F0 80..8F
      utf8_hi_ = 0xBF;
    } else if (lead == 0xF4) {
      utf8_lo_ = 0x80;
      utf8_hi_ = 0x8F;  // reject > U+10FFFF (F4 90..BF and beyond)
    } else {
      utf8_lo_ = 0x80;
      utf8_hi_ = 0xBF;
    }
  } else {
    return false;  // F5..FF always encode code points > U+10FFFF
  }
  return true;
}

void StreamParser::handleString(uint8_t byte) {
  switch (str_state_) {
    case StrState::Normal:
      if (utf8_remaining_ > 0) {
        // Middle of a multibyte UTF-8 sequence: the byte must be a valid
        // continuation in the constrained range. A structural byte such as '"'
        // or '\\' here means a truncated sequence and is rejected too.
        if (byte < utf8_lo_ || byte > utf8_hi_) {
          fail(ParseError::Grammar);  // invalid or truncated UTF-8 continuation
        } else {
          utf8_lo_ = 0x80;  // later continuation bytes are always 0x80..0xBF
          utf8_hi_ = 0xBF;
          --utf8_remaining_;
        }
        break;
      }
      if (byte == '"') {
        closeString();
      } else if (byte == '\\') {
        str_state_ = StrState::Escape;
      } else if (byte < 0x20) {
        fail(ParseError::Grammar);  // unescaped control character
      } else if (byte < 0x80) {
        if (emit_key_) {
          appendKeyChar(byte);
        }
      } else if (!beginUtf8(byte)) {
        fail(ParseError::Grammar);  // invalid UTF-8 lead byte
      } else if (emit_key_) {
        key_overflow_ = true;  // a non-ASCII key can never be the semantic "ac"
      }
      break;
    case StrState::Escape:
      switch (byte) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
          if (emit_key_) {
            appendKeyChar(unescapeSimple(byte));
          }
          str_state_ = StrState::Normal;
          break;
        case 'u':
          str_state_ = StrState::Unicode;
          hex_count_ = 0;
          hex_val_ = 0;
          break;
        default:
          fail(ParseError::Grammar);
          break;
      }
      break;
    case StrState::Unicode: {
      const int d = hexDigit(byte);
      if (d < 0) {
        fail(ParseError::Grammar);
        return;
      }
      hex_val_ = hex_val_ * 16 + static_cast<uint32_t>(d);
      if (++hex_count_ == 4) {
        if (hex_val_ >= 0xD800 && hex_val_ <= 0xDBFF) {
          pending_high_ = hex_val_;
          str_state_ = StrState::LowBackslash;
        } else if (hex_val_ >= 0xDC00 && hex_val_ <= 0xDFFF) {
          fail(ParseError::Grammar);  // lone low surrogate
        } else {
          if (emit_key_) {
            appendKeyChar(hex_val_);
          }
          str_state_ = StrState::Normal;
        }
      }
      break;
    }
    case StrState::LowBackslash:
      if (byte != '\\') {
        fail(ParseError::Grammar);  // unpaired high surrogate
      } else {
        str_state_ = StrState::LowU;
      }
      break;
    case StrState::LowU:
      if (byte != 'u') {
        fail(ParseError::Grammar);
      } else {
        str_state_ = StrState::LowUnicode;
        hex_count_ = 0;
        hex_val_ = 0;
      }
      break;
    case StrState::LowUnicode: {
      const int d = hexDigit(byte);
      if (d < 0) {
        fail(ParseError::Grammar);
        return;
      }
      hex_val_ = hex_val_ * 16 + static_cast<uint32_t>(d);
      if (++hex_count_ == 4) {
        if (hex_val_ < 0xDC00 || hex_val_ > 0xDFFF) {
          fail(ParseError::Grammar);  // invalid low surrogate
        } else {
          if (emit_key_) {
            const uint32_t cp = 0x10000 + ((pending_high_ - 0xD800) << 10) +
                                (hex_val_ - 0xDC00);
            appendKeyChar(cp);
          }
          str_state_ = StrState::Normal;
        }
      }
      break;
    }
  }
}

bool StreamParser::advanceNumber(uint8_t byte, bool* consumed) {
  *consumed = true;
  switch (num_state_) {
    case NumState::AfterMinus:
      if (byte == '0') {
        num_state_ = NumState::LeadZero;
      } else if (isDigit(byte)) {
        num_state_ = NumState::IntDigits;
      } else {
        return false;
      }
      return true;
    case NumState::LeadZero:
      if (byte == '.') {
        num_state_ = NumState::FracStart;
        return true;
      }
      if (byte == 'e' || byte == 'E') {
        num_state_ = NumState::ExpStart;
        return true;
      }
      if (isDigit(byte)) {
        return false;  // leading zero must not be followed by a digit
      }
      *consumed = false;
      return true;
    case NumState::IntDigits:
      if (isDigit(byte)) {
        return true;
      }
      if (byte == '.') {
        num_state_ = NumState::FracStart;
        return true;
      }
      if (byte == 'e' || byte == 'E') {
        num_state_ = NumState::ExpStart;
        return true;
      }
      *consumed = false;
      return true;
    case NumState::FracStart:
      if (isDigit(byte)) {
        num_state_ = NumState::FracDigits;
        return true;
      }
      return false;
    case NumState::FracDigits:
      if (isDigit(byte)) {
        return true;
      }
      if (byte == 'e' || byte == 'E') {
        num_state_ = NumState::ExpStart;
        return true;
      }
      *consumed = false;
      return true;
    case NumState::ExpStart:
      if (byte == '+' || byte == '-') {
        num_state_ = NumState::ExpSign;
        return true;
      }
      if (isDigit(byte)) {
        num_state_ = NumState::ExpDigits;
        return true;
      }
      return false;
    case NumState::ExpSign:
      if (isDigit(byte)) {
        num_state_ = NumState::ExpDigits;
        return true;
      }
      return false;
    case NumState::ExpDigits:
      if (isDigit(byte)) {
        return true;
      }
      *consumed = false;
      return true;
  }
  return false;
}

bool StreamParser::numberAtValidEnd() const {
  return num_state_ == NumState::LeadZero || num_state_ == NumState::IntDigits ||
         num_state_ == NumState::FracDigits || num_state_ == NumState::ExpDigits;
}

bool StreamParser::onAcElementStart() {
  if (ac_entry_count_ >= limits_.max_ac_entries) {
    fail(ParseError::TooLarge);
    return false;
  }
  current_element_ordinal_ = static_cast<uint16_t>(ac_entry_count_);
  ++ac_entry_count_;
  return true;
}

void StreamParser::finalizeCapturedObject() {
  ws_.object_buffer[capture_len_] = '\0';
  ObjectDecoderConfig config{show_ground_, limits_.max_depth};
  const DecodeResult result = decodeAircraftObject(
      ws_.object_buffer, capture_len_, ws_.arena, ws_.arena_size, config);
  switch (result.status) {
    case DecodeStatus::NoMemory:
      fail(ParseError::NoMemory);
      return;
    case DecodeStatus::Malformed:
      fail(ParseError::Grammar);
      return;
    case DecodeStatus::Skipped:
      return;
    case DecodeStatus::Accepted:
      selectAircraft(result.aircraft, result.lat, result.lon,
                     current_element_ordinal_);
      return;
  }
}

void StreamParser::selectAircraft(const Aircraft& aircraft, double lat,
                                  double lon, uint16_t ordinal) {
  if (limits_.max_aircraft == 0) {
    return;  // zero-retention: validate fully but keep no aircraft (no slots)
  }
  const float distance = haversineKm(center_lat_, center_lon_, lat, lon);
  if (count_ < limits_.max_aircraft) {
    const size_t slot = count_;
    ws_.aircraft[slot] = aircraft;
    ws_.distances[slot] = distance;
    ws_.ordinals[slot] = ordinal;
    ++count_;
    return;
  }

  // Replace the current farthest retained aircraft, breaking ties by keeping
  // the earlier source order (incoming ordinals always increase).
  size_t worst = 0;
  for (size_t i = 1; i < count_; ++i) {
    if (ws_.distances[i] > ws_.distances[worst] ||
        (ws_.distances[i] == ws_.distances[worst] &&
         ws_.ordinals[i] > ws_.ordinals[worst])) {
      worst = i;
    }
  }
  if (distance < ws_.distances[worst]) {
    ws_.aircraft[worst] = aircraft;
    ws_.distances[worst] = distance;
    ws_.ordinals[worst] = ordinal;
  }
}

void StreamParser::sortRetained() {
  for (size_t i = 1; i < count_; ++i) {
    const Aircraft key_ac = ws_.aircraft[i];
    const float key_d = ws_.distances[i];
    const uint16_t key_o = ws_.ordinals[i];
    size_t j = i;
    while (j > 0 && (ws_.distances[j - 1] > key_d ||
                     (ws_.distances[j - 1] == key_d &&
                      ws_.ordinals[j - 1] > key_o))) {
      ws_.aircraft[j] = ws_.aircraft[j - 1];
      ws_.distances[j] = ws_.distances[j - 1];
      ws_.ordinals[j] = ws_.ordinals[j - 1];
      --j;
    }
    ws_.aircraft[j] = key_ac;
    ws_.distances[j] = key_d;
    ws_.ordinals[j] = key_o;
  }
}

bool StreamParser::consume(uint8_t byte) {
  if (error_ != ParseError::None) {
    return false;
  }

  if (capturing_ && !appendCapture(byte)) {
    return false;
  }

  for (int guard = 0; guard < 2; ++guard) {
    bool reprocess = false;
    switch (state_) {
      case State::Root:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == '{') {
          pushContainer(true, false);
        } else {
          fail(ParseError::Grammar);  // the root must be a single object
        }
        break;
      case State::ObjKeyOrClose:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == '"') {
          startKeyString();
        } else if (byte == '}') {
          closeContainer(true);
        } else {
          fail(ParseError::Grammar);
        }
        break;
      case State::ObjKey:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == '"') {
          startKeyString();
        } else {
          fail(ParseError::Grammar);  // trailing comma or missing key
        }
        break;
      case State::Colon:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == ':') {
          state_ = State::ValueObj;
        } else {
          fail(ParseError::Grammar);
        }
        break;
      case State::ValueObj:
        if (isWhitespace(byte)) {
          break;
        }
        dispatchValue(byte);
        break;
      case State::ArrValueOrClose:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == ']') {
          closeContainer(false);
        } else {
          if (currentFrameIsArrayAc() && !onAcElementStart()) {
            break;
          }
          dispatchValue(byte);
        }
        break;
      case State::ArrValue:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == ']') {
          fail(ParseError::Grammar);  // trailing comma
        } else {
          if (currentFrameIsArrayAc() && !onAcElementStart()) {
            break;
          }
          dispatchValue(byte);
        }
        break;
      case State::AfterValue:
        if (isWhitespace(byte)) {
          break;
        }
        if (byte == ',') {
          state_ = frame_is_object_[depth_ - 1] != 0 ? State::ObjKey
                                                      : State::ArrValue;
        } else if (byte == '}') {
          closeContainer(true);
        } else if (byte == ']') {
          closeContainer(false);
        } else {
          fail(ParseError::Grammar);
        }
        break;
      case State::Str:
        handleString(byte);
        break;
      case State::Num: {
        bool consumed = false;
        if (!advanceNumber(byte, &consumed)) {
          fail(ParseError::Grammar);
        } else if (!consumed) {
          if (!numberAtValidEnd()) {
            fail(ParseError::Grammar);
          } else {
            state_ = State::AfterValue;
            reprocess = true;
          }
        }
        break;
      }
      case State::Lit:
        if (byte == static_cast<uint8_t>(lit_target_[lit_index_])) {
          ++lit_index_;
          if (lit_target_[lit_index_] == '\0') {
            state_ = State::AfterValue;
          }
        } else {
          fail(ParseError::Grammar);
        }
        break;
      case State::EndDoc:
        if (!isWhitespace(byte)) {
          fail(ParseError::Grammar);  // trailing data after the root object
        }
        break;
    }

    if (error_ != ParseError::None) {
      return false;
    }
    if (!reprocess) {
      break;
    }
  }

  if (capturing_ && depth_ == capture_base_depth_) {
    finalizeCapturedObject();
    capturing_ = false;
    capture_len_ = 0;
    if (error_ != ParseError::None) {
      return false;
    }
  }

  return true;
}

bool StreamParser::consume(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (!consume(data[i])) {
      return false;
    }
  }
  return error_ == ParseError::None;
}

bool StreamParser::finish() {
  if (error_ != ParseError::None) {
    return false;
  }
  if (state_ != State::EndDoc) {
    fail(ParseError::Grammar);  // truncated document
    return false;
  }
  if (!ac_seen_) {
    fail(ParseError::Grammar);  // no semantic top-level "ac" array
    return false;
  }
  sortRetained();
  return true;
}

}  // namespace services::adsb
