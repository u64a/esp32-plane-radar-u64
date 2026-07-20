#pragma once

// Arduino-free, token-aware streaming validator for the ADS-B response body. It
// fully validates JSON grammar (strings, escapes, surrogate pairs, numbers,
// literals, nesting, and trailing data), locates exactly one semantic top-level
// "ac" array, hands each object element to the bounded ArduinoJson decoder, and
// retains the nearest kMaxAircraft aircraft deterministically. It never includes
// ArduinoJson: object decoding is delegated through adsb_object_decoder.h.

#include <cstddef>
#include <cstdint>

#include "services/adsb_types.h"

namespace services::adsb {

// Cause of a fatal parse failure. Grammar maps to ParseError, TooLarge to
// ResponseTooLarge, and NoMemory to NoMemory at the fetch layer.
enum class ParseError : uint8_t {
  None,
  Grammar,
  TooLarge,
  NoMemory,
};

// Caller-owned fixed workspace. All pointers must remain valid for the parser's
// lifetime. aircraft/distances/ordinals hold at least limits.max_aircraft
// entries; object_buffer holds at least limits.max_object_bytes + 1 bytes.
struct ParserWorkspace {
  Aircraft* aircraft;
  float* distances;
  uint16_t* ordinals;
  char* object_buffer;
  size_t object_capacity;
  void* arena;
  size_t arena_size;
};

// Maximum container nesting the fixed parser stack can track.
inline constexpr uint8_t kParserDepthCap = 16;

class StreamParser {
 public:
  StreamParser(double center_lat, double center_lon, const ParseOptions& options,
               const ParserWorkspace& workspace);

  // Feed decoded body bytes. Returns false once a fatal error is recorded; all
  // subsequent calls are no-ops that also return false.
  bool consume(uint8_t byte);
  bool consume(const uint8_t* data, size_t length);

  // Signal end of body. Returns true only for a complete, fully validated
  // document containing exactly one top-level "ac" array.
  bool finish();

  ParseError error() const { return error_; }
  size_t count() const { return count_; }

 private:
  enum class State : uint8_t {
    Root,
    ObjKeyOrClose,
    ObjKey,
    Colon,
    ValueObj,
    ArrValueOrClose,
    ArrValue,
    AfterValue,
    Str,
    Num,
    Lit,
    EndDoc,
  };

  enum class StrState : uint8_t {
    Normal,
    Escape,
    Unicode,
    LowBackslash,
    LowU,
    LowUnicode,
  };

  enum class NumState : uint8_t {
    AfterMinus,
    LeadZero,
    IntDigits,
    FracStart,
    FracDigits,
    ExpStart,
    ExpSign,
    ExpDigits,
  };

  void fail(ParseError err);
  bool appendCapture(uint8_t byte);
  bool currentFrameIsArrayAc() const;
  bool pushContainer(bool is_object, bool is_ac);
  void closeContainer(bool expect_object);
  void startKeyString();
  void startValueString();
  void dispatchValue(uint8_t byte);
  void handleString(uint8_t byte);
  bool beginUtf8(uint8_t lead);
  void closeString();
  void appendKeyChar(uint32_t codepoint);
  bool advanceNumber(uint8_t byte, bool* consumed);
  bool numberAtValidEnd() const;
  bool onAcElementStart();
  void finalizeCapturedObject();
  void selectAircraft(const Aircraft& aircraft, double lat, double lon,
                      uint16_t ordinal);
  void sortRetained();

  double center_lat_;
  double center_lon_;
  ParseLimits limits_;
  bool show_ground_;
  ParserWorkspace ws_;

  State state_;
  StrState str_state_;
  NumState num_state_;
  ParseError error_;

  uint8_t frame_is_object_[kParserDepthCap];
  bool frame_is_ac_[kParserDepthCap];
  uint8_t depth_;

  bool ac_seen_;
  bool pending_key_is_ac_;
  uint8_t ac_array_depth_;

  bool str_is_key_;
  bool emit_key_;
  char key_buf_[2];
  uint8_t key_len_;
  bool key_overflow_;

  uint8_t hex_count_;
  uint32_t hex_val_;
  uint32_t pending_high_;

  // Allocation-free UTF-8 validation state for bytes inside a JSON string. When
  // utf8_remaining_ > 0 the parser is mid-sequence and the next continuation
  // byte must fall within [utf8_lo_, utf8_hi_]; that first-continuation range
  // encodes the overlong, surrogate, and > U+10FFFF constraints.
  uint8_t utf8_remaining_;
  uint8_t utf8_lo_;
  uint8_t utf8_hi_;

  const char* lit_target_;
  uint8_t lit_index_;

  bool capturing_;
  size_t capture_len_;
  uint8_t capture_base_depth_;
  uint32_t ac_entry_count_;
  uint16_t current_element_ordinal_;

  size_t count_;
};

}  // namespace services::adsb
