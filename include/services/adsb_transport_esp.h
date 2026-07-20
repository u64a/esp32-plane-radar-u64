#pragma once

// Thin ESP32 WiFiClientSecure adapter implementing the Arduino-free transport
// seams. TLS policy is intentionally isolated here so Phase 7 can add CA/SNTP
// verification without touching the HTTP decoder or parser. This header is
// ESP-only and must never be compiled into the native shared build.

#include <WiFiClientSecure.h>

#include "services/adsb_transport.h"

namespace services::adsb {

// Non-blocking byte source over an established TLS connection.
class EspTlsByteSource : public ByteSource {
 public:
  explicit EspTlsByteSource(WiFiClientSecure& client) : client_(client) {}
  ReadStatus read(uint8_t* buffer, size_t capacity, size_t* out_len) override;

 private:
  WiFiClientSecure& client_;
};

class EspMillisClock : public Clock {
 public:
  uint32_t nowMs() const override;
};

// Invokes the registered network poll callback during controllable idle waits.
class EspPollIdle : public IdleHandler {
 public:
  using PollFn = void (*)();
  explicit EspPollIdle(PollFn fn) : fn_(fn) {}
  void onIdle() override {
    if (fn_ != nullptr) {
      fn_();
    }
  }

 private:
  PollFn fn_;
};

// Resolve DNS (distinguishing DnsFailure), then perform the TCP + TLS connect
// against the resolved IP using the SNI-capable IP+host overload (no second DNS
// lookup). The connect budget (DNS + TCP + TLS) is cumulative: it starts before
// DNS, and only the time left after DNS bounds the socket + handshake. Because
// connect(IP,port,host,...) applies setTimeout() to the TCP select and a
// SEPARATE setHandshakeTimeout() to the handshake, that remainder is partitioned
// into non-overlapping whole-second TCP and TLS slices (favoring TLS) whose sum
// stays within the budget -- never the full remainder to both. If fewer than two
// whole seconds remain the split is not viable and the result is Timeout without
// starting connect(). A late success (connect() returns after the absolute
// budget is spent) is rejected: the socket is stopped and Timeout returned. A
// failure that consumes the whole budget is Timeout; an earlier failure is
// TlsFailure. Exception: WiFi.hostByName() has no timeout knob, so DNS may run up
// to the ESP-IDF resolver's ~15 s core timeout that connect_timeout_ms cannot
// preempt. This connect call blocks; the poll callback cannot run inside it.
// Phase 5 keeps the caller's explicit insecure policy (no CA); Phase 7 will pass
// CA material through the connect overload.
ConnectOutcome espTlsConnect(WiFiClientSecure& client, const char* host,
                             uint16_t port, uint32_t connect_timeout_ms);

// Send the whole request, tolerating partial writes and yielding on back-pressure.
bool espSendAll(WiFiClientSecure& client, const uint8_t* data, size_t length,
                Clock& clock, IdleHandler& idle, uint32_t deadline_ms);

}  // namespace services::adsb
