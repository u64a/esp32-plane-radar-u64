#include "services/adsb_transport_esp.h"

#include <Arduino.h>
#include <WiFi.h>
#include <mbedtls/ssl.h>  // MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY

#include "core/connect_budget.h"
#include "core/time_math.h"

namespace services::adsb {

// Embedded compile verification: the Arduino-free classification seam receives
// the platform's clean-shutdown sentinel as an argument. Confirm the pinned core
// still exposes it as a negative mbedTLS code so a peer close_notify can never
// collide with the non-negative available()/read() byte counts.
static_assert(MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY < 0,
              "close_notify sentinel must be a negative mbedTLS code");

ReadStatus EspTlsByteSource::read(uint8_t* buffer, size_t capacity,
                                  size_t* out_len) {
  const int available = client_.available();
  switch (classifyTlsAvailable(available, client_.connected(),
                               MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)) {
    case TlsReadAction::Error:
      return ReadStatus::Error;  // negative available(): real TLS receive failure
    case TlsReadAction::End:
      return ReadStatus::End;  // peer close_notify, or closed with nothing buffered
    case TlsReadAction::WouldBlock:
      return ReadStatus::WouldBlock;
    case TlsReadAction::Read:
      break;
  }
  size_t want = capacity;
  if (static_cast<size_t>(available) < want) {
    want = static_cast<size_t>(available);
  }
  const int n = client_.read(buffer, want);
  const ReadStatus st =
      classifyTlsReadResult(n, MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY);
  if (st == ReadStatus::Data) {
    *out_len = static_cast<size_t>(n);
  }
  return st;
}

uint32_t EspMillisClock::nowMs() const { return millis(); }

ConnectOutcome espTlsConnect(WiFiClientSecure& client, const char* host,
                             uint16_t port, uint32_t connect_timeout_ms) {
  // The connect budget (DNS + TCP + TLS) starts here, BEFORE DNS, so resolution
  // time is charged against it and the socket/handshake only get what remains.
  const uint32_t started = millis();

  // WiFi.hostByName() exposes no timeout knob: the ESP-IDF resolver enforces its
  // own uninterruptible ~15 s core timeout that connect_timeout_ms CANNOT
  // preempt. This is the single documented exception to the cumulative budget --
  // DNS may overrun it, but its elapsed time is still subtracted below.
  IPAddress address;
  if (!WiFi.hostByName(host, address)) {
    return ConnectOutcome::DnsFailure;
  }

  // Charge DNS against the budget. connect(IP,port,host,...) does NOT share one
  // deadline across TCP and TLS: it uses setTimeout() for the socket select and
  // then a SEPARATE setHandshakeTimeout() for the handshake, so handing the full
  // remainder to both would let a slow peer block nearly twice the budget.
  // Partition the post-DNS remainder into non-overlapping whole-second TCP and
  // TLS slices whose sum stays within the budget (favoring TLS). Fewer than two
  // whole seconds cannot yield a viable split, so classify Timeout WITHOUT even
  // starting connect(): the connect budget is effectively spent.
  const uint32_t remaining_ms =
      core::remainingBudgetMs(millis(), started, connect_timeout_ms);
  const core::ConnectBudgetSplit split = core::splitConnectBudget(remaining_ms);
  if (!split.viable) {
    return ConnectOutcome::Timeout;
  }
  client.setTimeout(split.tcp_seconds);  // TCP socket connect ceiling (s)
  client.setHandshakeTimeout(split.tls_seconds);  // TLS handshake ceiling (s)

  // Reuse the already-resolved address (NO second DNS lookup) while still
  // sending SNI via the public IP+host overload. Phase 5 policy is explicit
  // insecure with no fallback: the caller has called setInsecure(), so CA/cert/
  // key are null here. Phase 7 seam -- pass real CA material (and enable
  // verification) through these three arguments to add TLS trust later.
  const int ok = client.connect(address, port, host, nullptr, nullptr, nullptr);

  // Classify against the ORIGINAL absolute budget: a success reported only after
  // the budget is spent is a late success and must not be honored (it would
  // violate the advertised ceiling), and a failure that ran the budget out is a
  // Timeout rather than a TlsFailure. connect() reports only success/failure, so
  // this is the strongest truthful distinction available.
  const bool budget_exhausted =
      core::remainingBudgetMs(millis(), started, connect_timeout_ms) == 0;
  switch (core::classifyConnectCompletion(ok == 1, budget_exhausted)) {
    case core::ConnectCompletion::Connected:
      return ConnectOutcome::Connected;
    case core::ConnectCompletion::Timeout:
      if (ok == 1) {
        client.stop();  // tear down a late-success socket we are rejecting
      }
      return ConnectOutcome::Timeout;
    case core::ConnectCompletion::TlsFailure:
      return ConnectOutcome::TlsFailure;
  }
  return ConnectOutcome::TlsFailure;  // unreachable: switch is exhaustive
}

bool espSendAll(WiFiClientSecure& client, const uint8_t* data, size_t length,
                Clock& clock, IdleHandler& idle, uint32_t deadline_ms) {
  size_t sent = 0;
  const uint32_t started = clock.nowMs();
  while (sent < length) {
    if (deadline_ms != 0 &&
        core::elapsedAtLeast(clock.nowMs(), started, deadline_ms)) {
      return false;
    }
    if (!client.connected()) {
      return false;
    }
    const size_t n = client.write(data + sent, length - sent);
    if (n > 0) {
      sent += n;
    } else {
      idle.onIdle();  // yield/poll before retrying on back-pressure
    }
  }
  return true;
}

}  // namespace services::adsb
