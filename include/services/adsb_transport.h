#pragma once

// Arduino-free seams that decouple the bounded HTTP response decoder from the
// ESP WiFiClientSecure transport. Production wires these to real hardware; the
// native tests wire them to scripted fakes for the byte stream, clock, and idle
// pump. No ESP or Arduino headers appear here.

#include <cstddef>
#include <cstdint>

namespace services::adsb {

// Result of a non-blocking read from the transport.
enum class ReadStatus : uint8_t {
  Data,       // out_len bytes were produced
  WouldBlock, // no data yet; caller should idle and retry
  End,        // clean end of stream
  Error,      // transport error
};

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Read up to capacity bytes without blocking. On ReadStatus::Data, *out_len
  // is set to the number of bytes written (>= 1).
  virtual ReadStatus read(uint8_t* buffer, size_t capacity, size_t* out_len) = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint32_t nowMs() const = 0;
};

class IdleHandler {
 public:
  virtual ~IdleHandler() = default;
  // Poll/yield during idle waits so the registered network callback still runs.
  virtual void onIdle() = 0;
};

// Truthful transport connection outcome. Callers must not fabricate a
// DnsFailure/TlsFailure distinction the core cannot actually report.
enum class ConnectOutcome : uint8_t {
  Connected,
  DnsFailure,
  TlsFailure,
  Timeout,
};

// What a non-blocking TLS byte source should do after inspecting the client's
// reported available() count. Kept Arduino-free and header-only so the
// available()-vs-connected() classification can be unit-tested natively without
// a real WiFiClientSecure.
enum class TlsReadAction : uint8_t {
  Read,        // available > 0: bytes are ready, perform the read
  Error,       // available < 0: TLS receive failure (may have already stop()ped)
  End,         // clean end: peer close_notify, or closed with nothing buffered
  WouldBlock,  // available == 0 and still connected: no data yet
};

// Arduino-ESP32 2.0.14 WiFiClientSecure::available() surfaces the mbedTLS record
// layer's return code directly (and internally calls stop(), so connected()
// also becomes false) whenever that code is negative. A clean TLS shutdown is
// reported as MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY (-30848): it is the peer's
// orderly close, so a close-delimited body must finish as End rather than fail.
// Every OTHER negative value is a genuine receive failure that must not be
// mistaken for a clean EOF, so it maps to Error strictly before connected() is
// consulted. The platform sentinel is passed in by the ESP adapter (which
// includes <mbedtls/ssl.h>) so this seam stays Arduino-free and native-testable.
inline TlsReadAction classifyTlsAvailable(int available, bool connected,
                                          int close_notify_code) {
  if (available < 0) {
    return available == close_notify_code ? TlsReadAction::End
                                          : TlsReadAction::Error;
  }
  if (available > 0) {
    return TlsReadAction::Read;
  }
  return connected ? TlsReadAction::WouldBlock : TlsReadAction::End;
}

// Classify the byte count returned by a TLS read once available() reported
// readable data: >0 yields Data (caller sets out_len); a negative value equal to
// the peer close_notify sentinel is the same clean shutdown reported as End; any
// other negative is a transport Error; and 0 is a transient empty read to retry.
inline ReadStatus classifyTlsReadResult(int n, int close_notify_code) {
  if (n > 0) {
    return ReadStatus::Data;
  }
  if (n < 0) {
    return n == close_notify_code ? ReadStatus::End : ReadStatus::Error;
  }
  return ReadStatus::WouldBlock;
}

}  // namespace services::adsb
