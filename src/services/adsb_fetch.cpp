#include "services/adsb_fetch.h"

#include "services/adsb_stream_parser.h"

namespace services::adsb {

namespace {

// Body sink that feeds the streaming parser for a 200 response and drains
// (within the HTTP body limit) for any other status. It records the parser's
// error so runFetch can map a rejected body precisely.
class ParserBodySink : public BodySink {
 public:
  explicit ParserBodySink(StreamParser* parser) : parser_(parser) {}

  void begin(int status) override { parse_mode_ = status == 200; }

  bool write(const uint8_t* data, size_t length) override {
    if (!parse_mode_) {
      return true;  // drain non-200 bodies
    }
    return parser_->consume(data, length);
  }

  bool finish() override {
    if (!parse_mode_) {
      return true;
    }
    return parser_->finish();
  }

  bool parseMode() const { return parse_mode_; }
  ParseError parserError() const { return parser_->error(); }

 private:
  StreamParser* parser_;
  bool parse_mode_ = false;
};

FetchOutcome mapParserError(ParseError error) {
  switch (error) {
    case ParseError::TooLarge:
      return FetchOutcome::ResponseTooLarge;
    case ParseError::NoMemory:
      return FetchOutcome::NoMemory;
    case ParseError::Grammar:
    case ParseError::None:
    default:
      return FetchOutcome::ParseError;
  }
}

FetchOutcome mapStatus(int status) {
  if (status == 429) {
    return FetchOutcome::Http429;
  }
  if (status >= 500 && status <= 599) {
    return FetchOutcome::Http5xx;
  }
  return FetchOutcome::HttpOther;
}

}  // namespace

FetchResult runFetch(ByteSource& source, Clock& clock, IdleHandler& idle,
                     double center_lat, double center_lon,
                     const ParseOptions& parse_options,
                     const HttpLimits& http_limits,
                     const HttpDeadlines& deadlines,
                     const FetchWorkspace& workspace, AircraftSnapshot& out,
                     uint32_t revision) {
  ParserWorkspace parser_ws{};
  parser_ws.aircraft = out.aircraft;
  parser_ws.distances = workspace.distances;
  parser_ws.ordinals = workspace.ordinals;
  parser_ws.object_buffer = workspace.object_buffer;
  parser_ws.object_capacity = workspace.object_capacity;
  parser_ws.arena = workspace.arena;
  parser_ws.arena_size = workspace.arena_size;

  StreamParser parser(center_lat, center_lon, parse_options, parser_ws);
  ParserBodySink sink(&parser);

  const HttpResponseResult http = decodeHttpResponse(
      source, clock, idle, http_limits, deadlines, workspace.http, sink);

  FetchResult result{};
  result.outcome = FetchOutcome::ParseError;
  result.http_status = http.status;
  result.bytes_received = http.decoded_body_bytes;
  result.retry_after_ms = 0;
  result.aircraft_count = 0;

  switch (http.outcome) {
    case HttpOutcome::Timeout:
      result.outcome = FetchOutcome::Timeout;
      break;
    case HttpOutcome::TransportError:
      // A premature end or read error means no complete response was framed.
      result.outcome = FetchOutcome::ParseError;
      break;
    case HttpOutcome::ResponseTooLarge:
      result.outcome = FetchOutcome::ResponseTooLarge;
      break;
    case HttpOutcome::ParseError:
      result.outcome = FetchOutcome::ParseError;
      break;
    case HttpOutcome::BodyRejected:
      result.outcome = mapParserError(sink.parserError());
      break;
    case HttpOutcome::Ok:
      if (sink.parseMode()) {
        out.count = static_cast<uint16_t>(parser.count());
        out.source_revision = revision;
        result.outcome = FetchOutcome::Ok;
        result.aircraft_count = static_cast<uint16_t>(parser.count());
      } else {
        result.outcome = mapStatus(http.status);
        if ((http.status == 429 || http.status == 503) &&
            http.retry_after_present) {
          result.retry_after_ms = http.retry_after_ms;
        }
      }
      break;
  }
  return result;
}

}  // namespace services::adsb
