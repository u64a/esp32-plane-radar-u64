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
  result.retry_after_present = false;
  result.retry_after_ms = 0;
  result.aircraft_count = 0;
  // runFetch cannot see the peer certificate (that seam is ESP-only); leave the
  // authenticated floor candidate at 0 here. Only realFetch, which holds the
  // verified leaf notBefore, stamps it -- and only on a complete Ok.
  result.authenticated_cert_not_before_unix = 0;

  switch (http.outcome) {
    case HttpOutcome::Timeout:
      result.outcome = FetchOutcome::Timeout;
      break;
    case HttpOutcome::TransportError:
      // A premature end or read error means no complete response was framed.
      // This is a network read/EOF failure, not a grammar violation: report it
      // as a transient TransportFailure so a link interruption backs off briefly
      // instead of being mislabeled a permanent ParseError.
      result.outcome = FetchOutcome::TransportFailure;
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
        out.settings_revision = revision;
        result.outcome = FetchOutcome::Ok;
        result.aircraft_count = static_cast<uint16_t>(parser.count());
      } else {
        result.outcome = mapStatus(http.status);
        // Surface the parsed Retry-After to the poll policy. The presence bit
        // lets a header value of 0 (clamped to the rate minimum) be told apart
        // from an absent/malformed header (which uses the default wait).
        result.retry_after_present = http.retry_after_present;
        if (http.retry_after_present) {
          result.retry_after_ms = http.retry_after_ms;
        }
      }
      break;
  }
  return result;
}

int64_t authenticatedNotBeforeForResult(FetchOutcome outcome,
                                        int64_t leaf_not_before_unix) {
  // Only a complete, fully verified response (Ok) may carry the authenticated
  // floor candidate. Every other outcome -- a partial body, a transport/parse
  // failure, or a merely-connected result -- forces 0 so it can never ratchet the
  // persisted floor.
  return outcome == FetchOutcome::Ok ? leaf_not_before_unix : 0;
}

core::PollOutcome pollOutcomeFor(FetchOutcome outcome) {
  switch (outcome) {
    case FetchOutcome::Ok:
      return core::PollOutcome::Success;
    case FetchOutcome::Obsolete:
      return core::PollOutcome::Obsolete;
    case FetchOutcome::Timeout:
    case FetchOutcome::DnsFailure:
    case FetchOutcome::TlsFailure:
    case FetchOutcome::TransportFailure:
    case FetchOutcome::Http5xx:
    case FetchOutcome::TimeUnavailable:
      // TimeUnavailable is a "not ready yet" refusal: the main loop gates on
      // trusted time BEFORE calling the transport (serviceAdsb keeps the
      // immediate latch pending), so this only reaches here via a defense-in-
      // depth call site. Treat it as transient so it retries once time syncs,
      // rather than backing off hard or hot-looping.
      return core::PollOutcome::Transient;
    case FetchOutcome::Http429:
      return core::PollOutcome::RateLimited;
    case FetchOutcome::HttpOther:
    case FetchOutcome::ResponseTooLarge:
    case FetchOutcome::ParseError:
    case FetchOutcome::NoMemory:
    case FetchOutcome::CertInvalid:
      // CertInvalid is a hard trust failure (a peer cert invalid at trusted UTC,
      // or a MITM presenting an old/forged cert): back off hard rather than
      // hammering a misconfigured or hostile endpoint.
      return core::PollOutcome::Permanent;
  }
  return core::PollOutcome::Permanent;  // unreachable; keeps the compiler happy
}

core::PollOutcome pollOutcomeForPublish(PublishResult result) {
  switch (result) {
    case PublishResult::Published:
      return core::PollOutcome::Success;
    case PublishResult::ObsoleteRevision:
      return core::PollOutcome::Obsolete;
    case PublishResult::InvalidHandle:
    case PublishResult::NoCandidate:
      // A successful fetch that could not be published is an internal fault, not
      // a network result: never report Success, and back off hard (Permanent).
      return core::PollOutcome::Permanent;
  }
  return core::PollOutcome::Permanent;  // unreachable; keeps the compiler happy
}

bool fetchOutcomeNetworkAbortSensitive(FetchOutcome outcome) {
  switch (outcome) {
    case FetchOutcome::Timeout:
    case FetchOutcome::DnsFailure:
    case FetchOutcome::TlsFailure:
    case FetchOutcome::TransportFailure:
    case FetchOutcome::ParseError:
      return true;
    case FetchOutcome::Ok:
    case FetchOutcome::Http429:
    case FetchOutcome::Http5xx:
    case FetchOutcome::HttpOther:
    case FetchOutcome::ResponseTooLarge:
    case FetchOutcome::NoMemory:
    case FetchOutcome::Obsolete:
    case FetchOutcome::TimeUnavailable:
    case FetchOutcome::CertInvalid:
      // TimeUnavailable and CertInvalid are deterministic trust decisions, not
      // link-abort artifacts: a spurious Wi-Fi flap does not cause them, so they
      // must never be reinterpreted as a pause.
      return false;
  }
  return false;  // unreachable; keeps the compiler happy
}

core::PollOutcome effectiveOutcomeAfterFlap(FetchOutcome fetch_outcome,
                                            core::PollOutcome base_outcome,
                                            bool published_success,
                                            bool disconnect_flap) {
  if (!disconnect_flap || published_success) {
    // No flap, or a fully validated/published success: the base class stands.
    return base_outcome;
  }
  if (fetchOutcomeNetworkAbortSensitive(fetch_outcome)) {
    // A spurious mid-fetch drop plausibly aborted the read: treat as a pause so
    // it never advances the transient/permanent backoff streak.
    return core::PollOutcome::Obsolete;
  }
  return base_outcome;
}

}  // namespace services::adsb
