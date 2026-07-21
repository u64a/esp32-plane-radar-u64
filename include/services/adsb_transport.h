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
  // Cooperative cancellation query for a controllable fetch. Non-pure with a
  // default of false so every existing IdleHandler (the synchronous poll hook and
  // the native fakes) stays source-compatible and never cancels -- the decoder
  // and send loops preserve byte-for-byte behavior. A worker-backed handler
  // overrides it to abort an in-flight fetch on requestPause.
  virtual bool cancelled() { return false; }
};

// Arduino-free control block shared by the synchronous fetch and the future
// optional network worker (compile-time default OFF). It carries an OPTIONAL idle
// pump and an OPTIONAL cooperative cancellation predicate, each with an opaque
// context pointer. No std::function and no heap -- just two function pointers and
// two context pointers -- so it is safe to copy onto a worker task's stack. A
// null callback disables that capability: the synchronous path leaves `cancel`
// null so it never cancels, and `idle` null simply skips the yield.
struct FetchControl {
  using IdleFn = void (*)(void* ctx);
  using CancelFn = bool (*)(void* ctx);
  IdleFn idle = nullptr;
  CancelFn cancel = nullptr;
  void* idle_ctx = nullptr;
  void* cancel_ctx = nullptr;
};

// True only when a cancellation predicate is installed AND it reports cancelled.
// A control with a null predicate (the synchronous path) is never cancelled.
inline bool fetchControlCancelled(const FetchControl& control) {
  return control.cancel != nullptr && control.cancel(control.cancel_ctx);
}

// IdleHandler that routes onIdle()/cancelled() through a FetchControl. Header-only
// and Arduino-free so the ESP fetch and the native tests drive the exact same
// idle/cancel plumbing that the bounded HTTP decoder and the send loop consume.
class FetchControlIdle : public IdleHandler {
 public:
  explicit FetchControlIdle(const FetchControl& control) : control_(control) {}
  void onIdle() override {
    if (control_.idle != nullptr) {
      control_.idle(control_.idle_ctx);
    }
  }
  bool cancelled() override { return fetchControlCancelled(control_); }

 private:
  FetchControl control_;
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
