# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) · **Firmware:** [Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

## What it does

1. **Wi‑Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid, over **verified HTTPS** and only once **trusted UTC time** is established (see [Security: verified TLS & trusted time](#security-verified-tls--trusted-time-phase-7))

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop and refreshes on a **3 s completion‑relative** ADS‑B poll, backing off on errors and ageing to a stale/offline state when data stops arriving.

## Controls (BOOT, GPIO 9, active LOW)

| Action | Effect |
|--------|--------|
| **Short tap** | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Hold 3 s** | Clear Wi‑Fi, location, and units; reboot into setup portal |

During setup you can also hold BOOT at power-on to force a credential reset (same as the long press).

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`** — both are shown on the yellow setup screen; captive portal may open automatically
3. Set home Wi‑Fi, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (e.g. from your router or serial log at boot)
2. Change Wi‑Fi, location, units, or runway overlay; save

The same portal runs on the setup AP and on the device’s LAN IP while connected to Wi‑Fi. mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly; use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |

After a reset, the device reboots and shows the setup screen immediately (no “Connecting” loop on stale credentials).

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`.

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring** — red heading triangle, magenta speed vector (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within ADS-B fetch) — small **red dot on the screen rim** at the correct bearing (direction cue; not distance-accurate past the ring)
- **Tags** — placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

As range decreases (or aircraft approach), targets move inward; beyond-ring dots become full symbols when they cross the outer ring.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/`
- Fetch radius: `ui::radar::fetchRadiusKm()` — scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll schedule: **completion-relative** — the next fetch is timed from the previous fetch's *completion*, not its start, so a slow request never stacks. Normal cadence is `kAdsbSuccessIntervalMs` (**3 s**, equal to `kAdsbFetchIntervalMs`); errors back off (see **Poll backoff, freshness & offline** below)
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

**Transport timeouts** (`config.h`) are cumulative, not fresh per phase:

- `kAdsbConnectTimeoutMs` (8 s) budgets DNS + TCP + TLS **together**: the timer starts before DNS, and only the time left after DNS bounds the socket connect and TLS handshake. Because `WiFiClientSecure::connect(IP, port, host, …)` uses `setTimeout()` for the TCP select and a **separate** `setHandshakeTimeout()` for the handshake, the post-DNS remainder is split into **non-overlapping whole-second TCP and TLS slices** (a conservative ~40 % TCP / ~60 % TLS split favoring the slower handshake, ≥ 1 s each, summed ms ≤ the remainder) — never the full remainder handed to both. If fewer than two whole seconds remain the split is not viable and the connect is classified `Timeout` before it starts, and a *late* success (one reported only after the absolute budget is spent) is rejected with `stop()` + `Timeout` rather than accepted. The resolved IP is reused for an SNI-capable connect, so DNS runs once. **DNS exception:** `WiFi.hostByName()` exposes no timeout, so name resolution *alone* can take up to the ESP-IDF resolver's ~15 s core timeout, which this budget cannot preempt; this is the **only** phase not bounded by the split. Its elapsed time is still charged, so the socket/handshake get only what remains (a fetch that spends the whole budget on DNS returns `Timeout`).
- `kAdsbOverallTimeoutMs` (10 s) budgets the request send **and** the response decode together; the send draws from it first and only the remainder is passed to the decoder — neither gets a fresh 10 s.
- `kAdsbStallTimeoutMs` (5 s) is a response-local inactivity cap between received bytes.
- A clean TLS close (`close_notify`) that delimits a `Connection: close` body is treated as a normal end of stream, not a receive error. A **premature** end of stream or read error (a body cut short of its `Content-Length`/chunk framing) is a `TransportError`, mapped to a **transient** `TransportFailure` — a network read/EOF abort is never conflated with a genuinely malformed HTTP/JSON payload (which stays a permanent `ParseError`).

Worst case for one fetch: up to ~15 s of uninterruptible DNS *only if the resolver itself stalls*, then at most the remaining connect budget for TCP + TLS, then at most 10 s shared across send + response.

### Poll backoff, freshness & offline

The main loop schedules ADS-B polls **from each fetch's completion** and classifies the outcome (`config.h` mirrors the canonical `core::kDefaultAdsbPollPolicy`):

| Outcome | Next fetch | Transient streak |
|---------|-----------|------------------|
| Success (incl. empty aircraft list) | `kAdsbSuccessIntervalMs` (3 s) | reset to 0 |
| Transient — timeout / DNS / TLS / transport read-EOF / HTTP 5xx | shared backoff `5 → 10 → 20 → 40 → 60 → 60 s` (`kAdsbTransientInitialMs`…`kAdsbTransientCapMs`) | +1 (saturating) |
| Rate-limited — HTTP 429 | `Retry-After` clamped to `[kAdsbRetryAfterMinMs, kAdsbRetryAfterMaxMs]` (5 s…5 min), else `kAdsbRateDefaultMs` (60 s) | untouched |
| Permanent — other HTTP / too-large / parse / no-memory | `kAdsbPermanentBackoffMs` (5 min) | untouched |
| Obsolete — a successful response discarded because the query revision changed mid-flight | success cadence (3 s) | untouched |

Only a genuine success resets the shared transient streak. A **range** or **location** change forces one immediate fetch for the new query revision (miles/runway toggles are visual-only and never do); the in-flight response for the old revision is discarded, and the previously published snapshot is preserved and hidden until fresh data arrives.

**Freshness** is measured from the last successful publish for the current query revision:

- **Live** → **Stale** at `kRadarStaleMs` (15 s): targets are retained and flagged `STALE <age>s`.
- **Stale** → **Offline** at `kRadarOfflineMs` (60 s): targets are hidden; the grid remains.
- **Loading** (no success yet) becomes **Offline** at 60 s. A published zero-aircraft list is **Live** (not Offline).

**Wi-Fi drop** pauses fetches but never hides the radar: the last targets keep showing with a `NO WIFI` badge and age to Stale/Offline normally, and a drop is **not** counted as an ADS-B failure. On reconnect the radar redraws and one immediate fetch is forced (the transient streak is preserved). A drop that occurs **and auto-reconnects entirely inside one blocking fetch** is caught even though `WiFi.status()` reads connected before and after: a filtered `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` callback advances a lock-free 32-bit disconnect counter (registered once before network setup; the callback only bumps the atomic — no logging, drawing, or allocation), and the loop compares that counter before each fetch with its value after all post-fetch pumps. A detected mid-fetch flap forces one immediate refresh afterward (surviving the in-flight completion even if the link is already back), and — unless the fetch fully published a success — a link-interruption-sensitive outcome (timeout / DNS / TLS / transport read-EOF, and a plausibly-truncated parse error) is resolved as a pause so the spurious drop never advances the transient or permanent backoff. If the link is still down, polling stays paused and the immediate latch survives until reconnect. The frame is redrawn only on real changes — initial display, a settings change, a Wi-Fi edge, a fetch/publish, or a freshness transition — plus the `LOADING` dots animating every 500 ms and the `STALE` age ticking each second; Live/Offline frames are not redrawn continuously.

## Security: verified TLS & trusted time (Phase 7)

ADS-B is fetched over **HTTPS with full certificate verification**, and the device
**refuses to make any ADS-B connection until it has established trusted UTC time**
after boot.

### Network egress (exact)

| Purpose | Protocol / port | Host |
|---------|-----------------|------|
| ADS-B data | **HTTPS — TCP/443** | `opendata.adsb.fi` (Cloudflare-fronted) |
| Time sync | **SNTP — UDP/123** | `time.cloudflare.com` (default, the **only** time party) |

`time.cloudflare.com` is the sole default SNTP server because `adsb.fi` is already
behind Cloudflare, so it adds no additional operator. Up to **two** optional
compile-time fallback SNTP servers may be set (`kSntpServerFallback1/2` in
`config.h`, empty by default). **DHCP-provided NTP is disabled** (the SDK ships
`CONFIG_LWIP_DHCP_GET_NTP_SRV=1`, so the firmware explicitly calls
`esp_sntp_servermode_dhcp(false)`). There are **no other default outbound
parties** beyond DNS for the two hosts above.

### Verified TLS (no insecure fallback)

- The connection uses `WiFiClientSecure::connect(IP, port, host, CA, …)` with the
  pinned CA bundle passed as the CA argument and **`setInsecure()` is never
  called**, so pinned Arduino-ESP32 2.0.14 mbedTLS runs with
  `MBEDTLS_SSL_VERIFY_REQUIRED` and verifies **both the CA chain and the
  hostname** (`host` drives SNI and CN/SAN verification). The IP+host overload is
  used deliberately so the certificate is checked against the **hostname**, not
  the resolved IP. There is **no** insecure fallback, fingerprint-only path, or
  trust-on-first-use — a chain or hostname failure returns `TlsFailure` and the
  request body is never sent.
- **mbedTLS date limitation + explicit full-chain enforcement:** the pinned
  ESP32-C3 SDK is built **without** `CONFIG_MBEDTLS_HAVE_TIME_DATE`, so the
  handshake enforces neither the leaf's **nor any intermediate's**
  `notBefore`/`notAfter`. After the verified handshake and **before** sending any
  HTTP, the firmware re-derives the trusted timestamp (see below) and explicitly
  walks the **full retained peer chain** — the leaf plus every peer-supplied
  intermediate/cross-cert, via `mbedtls_x509_crt::next` from
  `getPeerCertificate()` — classifying **every** node against that single trusted
  UTC and **failing closed** (`CertInvalid`) if the chain is missing/empty, **any**
  node is not-yet-valid, expired, or malformed (including an inverted window), or
  the chain is longer than a conservative fixed bound (`core::kMaxPeerChainLen` =
  8, guarding against a cyclic/over-long `::next` list). The parser and send path
  are never reached. The walk reads the head pointer **once** and performs **no
  allocation and no further SSL call** while traversing. The CA-signed **leaf**
  `notBefore` is exposed as the persisted-floor candidate **only when the whole
  chain is valid**, so an invalid intermediate under a valid leaf forces the floor
  candidate to 0 and can never ratchet NVS. The trusted timestamp used for this
  check is obtained **again after DNS + TCP + TLS**, never a value captured before
  the connection existed (closing the callback/pre-handshake staleness window); if
  trust expired or was revoked while the blocking handshake ran, the fetch returns
  `TimeUnavailable` before any HTTP is sent. The pure, Arduino-free comparison and
  the bounded chain accumulator live in `core/cert_time.h` and are native-tested:
  they fail closed on ASN.1 seconds outside `0..59` and treat an inverted window
  (`notBefore > notAfter`) as `Malformed`.
- **Trust anchor (pinned root) handling:** this runtime check enforces the dates
  of what the **peer supplied**. `getPeerCertificate()` does **not** expose the
  locally pinned root that actually terminated chain verification, so the firmware
  makes **no** claim of runtime enforcement of the pinned root's own dates.
  Instead, the pinned roots' identity (SHA-256 DER pin), self-issued naming, and a
  maintenance expiry horizon are guaranteed **deterministically offline** by
  `scripts/verify-ca-bundle.ps1` (run as a build gate). Chain and hostname
  verification *against* those pinned roots still happens in the handshake under
  `MBEDTLS_SSL_VERIFY_REQUIRED`; the runtime step above adds the peer-chain date
  enforcement the pinned mbedTLS build omits.

### CA trust bundle (provenance & maintenance)

`src/services/adsb_ca_bundle.cpp` embeds the **smallest stable explicit** bundle
of official self-signed roots in flash/rodata, covering the GTS and Let's Encrypt
RSA/ECDSA chains **currently observed** for this Cloudflare-fronted endpoint
(including the LE X1/X2 hierarchy). It is **not** claimed to cover every possible
future Cloudflare CA rotation: an unexpected rotation to a root outside this set
**fails closed** (`TlsFailure`, no request sent) and requires a bundle update.

| Root | Key | Source | SHA-256 (DER) |
|------|-----|--------|---------------|
| ISRG Root X1 | RSA | `https://letsencrypt.org/certs/isrgrootx1.pem` | `96:BC:EC:06…08:C6` |
| ISRG Root X2 | ECDSA | `https://letsencrypt.org/certs/isrg-root-x2.pem` | `69:72:9B:8E…14:70` |
| GTS Root R1 | RSA | `https://pki.goog/repo/certs/gtsr1.pem` | `D9:47:43:2A…F4:CF` |
| GTS Root R4 | ECDSA | `https://pki.goog/repo/certs/gtsr4.pem` | `34:9D:FA:40…3C:7D` |

`scripts/verify-ca-bundle.ps1` is a **deterministic, offline** gate (Windows
PowerShell 5.1 and pwsh 7) that **never downloads or mutates trust**: it parses
every committed certificate and checks the exact count (4), subjects, **self-
issued naming** (subject == issuer), the expected **SHA-256 DER fingerprints**
(the authoritative identity pin), and validity, and **fails if the earliest
root's expiry is within a maintenance horizon** (`-MinRootValidityDays`, default
365). The fingerprint pins are the identity guarantee — the gate does **not**
claim to cryptographically verify each root's self-signature. It also confirms
**both** `src/` and `include/` contain **no `setInsecure()`** call, that the
pinned CA is actually passed through the IP+host connect overload, that the
trusted-time adapter uses the derived accepted-sample clock (**no raw
`time(nullptr)`**), that `main` feeds the **CA-authenticated peer-`notBefore`
field** (`authenticated_cert_not_before_unix`) into the persisted-floor service
(and the superseded SNTP-time ratchet call is gone), and that the persisted-floor
ratchet reads **no** derived/SNTP clock for its NVS write. It also proves the ESP
transport still walks the **full peer chain** — the bounded `mbedtls_x509_crt`
`->next` traversal fed through the `certChain{Begin,AddNode,Finalize}` accumulator
— so a regression to a leaf-only date check (or a removed chain walk) fails the
gate. An optional `-LiveAdvisory` switch reports the live leaf chain but **never
gates builds or changes trust**. To update the roots, re-download from the
official sources above, refresh the pins in the gate, and re-run it.

### Trusted UTC time (fail-closed)

Time trust is an Arduino-free policy core (`core/time_trust.h`) driven by a thin
ESP adapter (`services/timekeeper.h`):

- **Release epoch floor** — a deterministic committed constant
  (`kReleaseEpochFloorUnix` = `1784505600`, 2026-07-20T00:00:00Z; overridable with
  `-DPLANE_RADAR_RELEASE_EPOCH`), **not** `__DATE__`/`__TIME__`. It is the lower
  bound on plausible "now".
- **SNTP** — the pinned non-blocking `configTime` path (never blocking
  `getLocalTime()` in the loop) with a minimal sync-notification callback that
  only latches the 64-bit sample under the repository's `portMUX_TYPE`
  critical-section pattern (no single-core-only weak-ordering assumption, no torn
  read). A non-blocking state machine (waiting for Wi-Fi → sync pending → trusted
  → retry/backoff) uses rollover-safe `millis()` deltas and never restarts SNTP in
  a hot loop.
- **Derived monotonic clock** — trusted "now" is the last **accepted** sample plus
  the rollover-safe `millis()` elapsed since it was accepted (`core::
  derivedTrustedNowUnix`), **never** the mutable system wall clock (`::time`).
  lwIP applies `settimeofday` *before* the sync callback runs — even for a sample
  the core later rejects — so gating trust on the wall clock would let an
  unconsumed/rejected sample race trusted state. An accepted sample older than the
  **trusted-sample max age** (12 h) is revoked and SNTP re-armed. `nowUnix()` and
  the certificate date check read only this derived clock; the persisted-floor
  ratchet reads **no** clock at all (its candidate is the CA-signed certificate
  `notBefore`).
- **Sample validation** — every sample (including later resyncs) is accepted only
  within `[floor − rollback_tolerance, floor + future_ceiling]` (**5 min** / ~10 y).
  Rejected samples leave/return the service **untrusted** and block fetches; a
  later rejected resync **revokes** trust. Trusted time survives a brief Wi-Fi
  drop (the derived clock keeps advancing), but **every boot must obtain a fresh
  accepted sample before the first ADS-B connection**. The generous ~10 y future
  ceiling is safe because forward time cannot be *persistently* poisoned (see the
  persisted floor) — the **CA-authenticated certificate notBefore**, not the
  ceiling, is the security boundary, proven by the native ratchet tests.
- **Persisted floor (NVS)** — only a **CA-authenticated monotonic lower bound** is
  stored (never treated as current time), in a dedicated `timefloor` namespace, as
  a single **versioned, checksummed blob** (`core::PersistedFloorRecord`: version +
  floor + FNV-1a integrity) written atomically. On read a corrupt/partial/wrong-
  version/future-invalid record falls back safely to the release floor (never
  weakened). The floor value is the **CA-signed peer leaf certificate's
  `notBefore` epoch** captured from a **complete** CA + hostname + date verified
  ADS-B response (never a mere TCP/TLS connect, and **never an SNTP-derived
  timestamp**). It is written only when that authenticated `notBefore` advances the
  stored floor by **at least 24 h**, so it updates **no more frequently than 24 h
  of authenticated floor advancement**: a same, older, or concurrently-served
  alternate certificate neither writes nor rolls the floor back, and a genuine
  certificate rotation advances it **once**, exactly to the new `notBefore`.
  Because an unauthenticated NTP attacker cannot choose a CA-signed `notBefore`, a
  spoofed SNTP sample can cause only a **non-persistent, in-session DoS** (blocked
  or delayed fetches) — **it cannot poison NVS**. An in-session flash-wear guard
  bounds writes per session; there is **no** attacker-controlled cross-reboot time
  throttle. An NVS write failure is surfaced but never weakens trust or blocks
  networking.
- **Recovery** — the credential/factory-reset path (`resetWifiCredentials()`)
  clears the `timefloor` namespace so a user can recover from corrupted/poisoned
  trust metadata; the reset reports honestly and does **not** claim success if the
  floor namespace cannot be cleared.
- **Fail-closed everywhere** — unavailable, stale/rollback, corrupt, or
  implausibly-future time **blocks ADS-B fetches** and **never** triggers insecure
  TLS. `serviceAdsb()` holds its immediate-fetch latch pending while untrusted and
  fires immediately once trusted; the transport itself also refuses to connect
  when untrusted (`TimeUnavailable`) so no future call site can bypass the gate.

### Hardware-only acceptance gates

Native tests and the offline CA gate prove the deterministic logic, but the real
TLS handshake over Wi-Fi, the **TLS verification heap peak / largest-free-block**
impact, live SNTP behavior, RF, and panel output remain **hardware-only** gates.
Do not treat a green native/offline run as proof of on-device TLS.

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | connect attempts, reconnect grace, portal timeout (`0` = no timeout) |
| BOOT | `kBootPin`, `kBootResetHoldMs`, `kBootTapMinMs` |
| Display SPI | pins, `kDisplayInvert`, `kDisplayRgbOrder`, `kDisplaySpiWriteHz` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until portal overrides) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbShowGroundAircraft`; cumulative transport budgets `kAdsbConnectTimeoutMs` / `kAdsbOverallTimeoutMs` / `kAdsbStallTimeoutMs`; poll backoff `kAdsbSuccessIntervalMs` / `kAdsbTransientInitialMs` / `kAdsbTransientCapMs` / `kAdsbRateDefaultMs` / `kAdsbRetryAfterMinMs` / `kAdsbRetryAfterMaxMs` / `kAdsbPermanentBackoffMs`; freshness `kRadarStaleMs` / `kRadarOfflineMs` |
| TLS / time (Phase 7) | `kReleaseEpochFloorUnix` (release floor; `-DPLANE_RADAR_RELEASE_EPOCH`), acceptance policy `kTimeRollbackToleranceSec` (5 min) / `kTimeFutureCeilingSec` (~10 y) / `kTimeSyncTimeoutMs` / `kTimeRetryBackoffInitialMs` / `kTimeRetryBackoffMaxMs` / `kTimeTrustedSampleMaxAgeMs` (12 h); SNTP `kSntpServerPrimary` + `kSntpServerFallback1/2`; CA-authenticated persisted floor `kTimeFloorNvsNamespace` / `kTimeFloorNvsKey` (versioned blob) / `kTimeFloorPersistIntervalMs` (in-session flash-wear guard) / `kTimeFloorMinAdvanceSec` (24 h authenticated advance per write). CA bundle: `src/services/adsb_ca_bundle.cpp` (verified by `scripts/verify-ca-bundle.ps1`) |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    lgfx_config.hpp
    display.h
    display_font.h
  data/
    large_airports.h
  ui/
    radar_theme.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```

## Wiring (GC9A01 ↔ ESP32-C3 Super Mini)

| Display | ESP32-C3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO **0** |
| CS | GPIO **1** |
| DC | GPIO **10** |
| SDA (MOSI) | GPIO **3** |
| SCL (SCLK) | GPIO **4** |
| BOOT (user) | GPIO **9** |

## Build

PlatformIO Core is pinned for local/scripted builds:

```powershell
py -m pip install -r requirements-dev.txt
```

```bash
pio run -t upload
pio device monitor
```

- PlatformIO env: **`supermini`**
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

For a clean Windows build that deletes the project `.pio` directory first:

```powershell
.\scripts\clean-build.ps1
```

### Native tests

Native tests use PlatformIO's Unity runner and do not require ESP32 hardware or
firmware libraries:

```powershell
py -m pip install -r requirements-dev.txt
.\scripts\setup-native-toolchain.ps1
.\scripts\native-test.ps1
```

The setup step explicitly downloads the pinned, signed w64devkit release into
`$env:LOCALAPPDATA\esp32-plane-radar\native-toolchains`. It verifies the asset
SHA-256, Authenticode signature, and GCC version before caching it. The test script
uses that compiler only for its child PlatformIO process; it does not change the
user or system `PATH`. No Arduino or ESP32 packages are linked into native tests.

The suite currently comprises **277 test cases across 21 native suites** (all
passing), including `test_time_trust` (26 cases: trusted-time state machine,
derived monotonic clock, stale-sample revoke, versioned persisted-floor record,
and the CA-authenticated certificate-`notBefore` floor ratchet) and
`test_cert_time` (17 cases: fail-closed certificate-date parsing, RFC 5280
validity, CA-signed `notBefore` extraction, and bounded full-chain
peer-certificate validation — expired/future/malformed intermediates, empty and
over-long chains) for the Phase 7 trust logic.

The friend-seam gate and the **offline CA trust gate** are pure PowerShell
(Windows PowerShell 5.1 and pwsh 7), need no ESP32 toolchain, and never touch the
network:

```powershell
.\scripts\check-native-test-access-gate.ps1   # SnapshotStoreTestAccess cannot leak into firmware
.\scripts\verify-ca-bundle.ps1                # 4 pinned roots, no setInsecure, CA enforced, derived-time clock, CA-authenticated cert-notBefore floor
```

### Memory budget

Measured with a pinned clean `supermini` build (`scripts\clean-build.ps1`), Phase 5
bounded ADS-B transport and parser implemented, against the `ea3039f` baseline:

| Build measurement | Baseline `ea3039f` | Phase 5 | Delta |
|-------------------|------:|------:|------:|
| Linker-reported static RAM | 50,924 | 61,460 | +10,536 |
| Linker-reported firmware flash | 1,241,252 | 1,231,868 | −9,384 |
| `firmware.bin` image | 1,305,200 | 1,296,816 | −8,384 |
| `firmware-merged.bin` image | 1,370,736 | 1,362,352 | −8,384 |

**Phase 6 runtime integration** (deterministic `RadarDisplayModel` frames, completion-relative
poll backoff, data freshness, and the settings query/visual seams) adds only a small,
fixed cost over Phase 5, measured with the same pinned clean build:

| Build measurement | Phase 5 | Phase 6 | Δ vs Phase 5 |
|-------------------|------:|------:|------:|
| Linker-reported static RAM | 61,460 | 61,588 | +128 |
| Linker-reported firmware flash | 1,231,868 | 1,236,204 | +4,336 |
| `firmware.bin` image | 1,296,816 | 1,302,432 | +5,616 |
| `firmware-merged.bin` image | 1,362,352 | 1,367,968 | +5,616 |

The +128 B static RAM is a handful of runtime latches (freshness, poll, reconnect,
last-rendered frame-key state, and the lock-free Wi-Fi disconnect-sequence counter); there
is **no** new per-frame or per-fetch allocation. The
radar still composites into **exactly one** 240×240 RGB565 frame sprite (heap-allocated once),
and each frame is a `RadarDisplayModel` passed by const reference — the published aircraft are
referenced through a copy-free `SnapshotView` (pointer + count + revision), never copied. The
deepest render frame is the pre-existing `drawAircraftFromSnapshot` (≈2.0 KB of on-stack
nearest-64 ranking arrays); the new main-loop/render frames are small (`loop` 128 B,
`renderIfNeeded` 128 B, `renderFrame` 160 B, `radarDisplayDraw` 80 B, measured via
`-fstack-usage`), so the application stack stays well within the Arduino loop-task budget.
Wi-Fi/TLS heap peaks and largest-free-block behavior remain hardware-only measurements.

**Phase 7 verified TLS + trusted time** (the pinned four-root CA bundle, the
Arduino-free SNTP trusted-UTC state machine, post-handshake full-chain
`notBefore`/`notAfter` enforcement, and the versioned persisted time-floor) adds
only a small, fixed cost over Phase 6, measured with the same pinned clean
`supermini` build:

| Build measurement | Phase 6 | Phase 7 | Δ vs Phase 6 |
|-------------------|------:|------:|------:|
| Linker-reported static RAM | 61,588 | 61,788 | +200 |
| Linker-reported firmware flash | 1,236,204 | 1,261,268 | +25,064 |
| `firmware.bin` image | 1,302,432 | 1,328,480 | +26,048 |
| `firmware-merged.bin` image | 1,367,968 | 1,394,016 | +26,048 |

The +200 B static RAM is the fixed trusted-time state (`core::TimeTrustState`, the
SNTP sample latch and its `portMUX`, and a one-shot write-warning flag); there is
**no** new heap or per-fetch allocation — the persisted-floor record is a 16-byte
on-stack buffer, and the new firmware frames stay small (measured via
`-fstack-usage`: `realFetch` 592 B including its pre-existing 256 B request buffer,
`espVerifyPeerCertValidity` 96 B for the bounded full peer-chain date walk (the
per-node accumulator adds 32 B and each `certChain*` step is 0–32 B),
`noteVerifiedCertFloor` 80 B including the 16 B record buffer, `timeTrustStep` 32 B,
the SNTP latch callback 16 B, `derivedTrustedNowUnix`/`shouldRatchetPersistedFloor`
0 B). The flash growth is dominated by the ~5.4 KB pinned CA bundle (PEM array, ~4 KB DER payload) in rodata plus
the mbedTLS chain/hostname verification and SNTP code pulled in by verified TLS;
the bounded full-chain date walk adds only ~172 B of flash over a leaf-only check.
The single 240×240 RGB565 framebuffer is unchanged, and the real Wi-Fi/TLS heap
peak remains a hardware-only gate.

Flash shrank because Arduino `HTTPClient`, its dynamic-`String` header parser, and the
whole-payload `String`/`JsonDocument` path were removed. The static RAM increase is the
fixed, caller-owned Phase 5 workspace (no per-response allocation, no second framebuffer):

| Fixed workspace (static BSS) | Bytes | Basis |
|------------------------------|------:|-------|
| Two `AircraftSnapshot` buffers | 6,160 | `2 × sizeof(AircraftSnapshot)` (double-buffered publish) |
| ArduinoJson per-object arena | 4,096 | Bounded allocator; `NoMemory` is explicit |
| Per-object JSON buffer | 2,049 | `kMaxObjectBytes (2,048) + NUL` |
| HTTP line/header buffer | 513 | Largest header-line limit `+ NUL` |
| Transport read scratch | 512 | One TLS read unit |
| Distance side array (`float[64]`) | 256 | Nearest-64 ranking |
| Source-ordinal side array (`uint16_t[64]`) | 128 | Deterministic tie-break |
| Buffer indices / revision / poll hook | ~16 | Publish index switch + poll callback |
| Single 240×240 RGB565 frame sprite | 115,200 | Required; still exactly one, heap-allocated once |

The two snapshots (6,160 bytes) replace the previous single `Aircraft[64]` array
(3,072 bytes), so the net static increase attributable to the new fixed buffers is
+10,536 bytes as measured above. Every large buffer is static and caller-owned, so the
per-fetch **application** stack stays well under 1 KB (small frames plus the parser's
fixed 16-entry container stack); the only deeper transient stack is ArduinoJson's
per-object recursion, which is bounded by the JSON depth limit of 16. Wi-Fi/TLS heap
peaks, fragmentation, and largest-free-block behavior remain hardware-only measurements.

### Web-flashable release image

Single `.bin` for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools (ESP32-C3, 4 MB, flash at **0x0**):

```bash
chmod +x scripts/merge-firmware.sh   # once
./scripts/merge-firmware.sh
```

Writes `release/plane-radar-merged.bin`. Skip rebuild if firmware is already built:

```bash
./scripts/merge-firmware.sh --no-build
```

Or via PlatformIO only (output: `.pio/build/supermini/firmware-merged.bin`):

```bash
pio run -e supermini
pio run -t merge -e supermini
```

Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
