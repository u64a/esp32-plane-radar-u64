# Plane Radar

<img width="800" height="450" alt="plane-radar" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

**Original project:** [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) · **Original releases:** [GitHub Releases](https://github.com/MatixYo/ESP32-Plane-Radar/releases) · **3D printed case (STL + assembly):** [MakerWorld](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083)

Firmware for an **ESP32-C3 Super Mini** and a **1.28″ round GC9A01** display (240×240). Shows a circular **ADS-B radar** around your configured location, with a **temporary, secured Wi‑Fi setup portal** for first-time setup.

## Project origin and differences

This repository is a hardened derivative of
[MatixYo's ESP32 Plane Radar](https://github.com/MatixYo/ESP32-Plane-Radar).
The original concept, hardware integration, display design, and firmware
baseline are credited to MatixYo. The original MIT copyright and permission
notice are retained in [LICENSE](LICENSE). This repository is independently
maintained and is not an official upstream release.

Major differences from the upstream baseline:

| Area | This repository |
|------|-----------------|
| Wi‑Fi provisioning | Replaces WiFiManager provisioning with a temporary WPA2 captive portal, a per-session password shown only on the display, CSRF protection, bounded routes, credential trial-before-commit, and power-loss-safe settings transactions |
| Network security | Adds pinned CA roots, hostname and certificate verification, trusted SNTP time, a CA-authenticated persisted time floor, and fail-closed TLS behavior |
| ADS-B handling | Adds bounded HTTP/JSON decoding, deterministic nearest-64 retention, strict transport limits, completion-relative polling, retry backoff, and stale/offline lifecycle states |
| Runtime and display | Corrects radar/runway geometry, adds range/unit/runway settings, redraw-on-change behavior, and an optional evaluation-only ADS-B worker while keeping the default firmware synchronous |
| Privacy and attack surface | Restricts runtime egress to ADS-B HTTPS, SNTP, and LAN-provided DNS/DHCP; removes OTA, mDNS, telemetry, and permanent LAN listeners |
| Verification and releases | Adds 606 native/headless test cases, 21 reviewed rendering goldens, policy/tamper gates, pinned build variants, reproducible firmware artifacts, package verification, and a hardware-acceptance checklist |

## What it does

1. **Wi‑Fi setup** (if needed) — a **temporary WPA2‑protected** captive portal on a MAC‑derived AP (**`PlaneRadar-XXYYZZ`**) whose one‑time password is shown **only on the device screen**
2. **Radar** — live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid, over **verified HTTPS** and only once **trusted UTC time** is established (see [Security: verified TLS & trusted time](#security-verified-tls--trusted-time-phase-7))

After Wi‑Fi is saved, the device reconnects automatically; the radar runs in the main loop and refreshes on a **3 s completion‑relative** ADS‑B poll, backing off on errors and ageing to a stale/offline state when data stops arriving. There is **no permanent LAN listener, no mDNS, and no OTA** — the setup portal exists only while a setup session is active.

## Controls (BOOT, GPIO 9, active LOW)

A single button drives three intents through the approved two‑stage gesture (never a fixed 3‑second reset, and no power‑on hold):

| Action | Effect |
|--------|--------|
| **Short tap** (40 ms – 1 s) | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Hold 2–8 s, release** | Open the secure Wi‑Fi setup portal |
| **Hold to 8 s (armed), release, then a separate 3 s hold within 10 s** | Factory erase (Wi‑Fi, location, range, units, runways, and the persisted time‑floor). The **same** hold can never erase — the 8 s hold only **arms** erase; it must be released, and then a fresh press held **3 s** within a **10 s** confirm window completes it, so a stuck button can’t wipe the device |

The screen shows progressive prompts during a hold (“Release now to configure Wi‑Fi”, then “Keep holding to arm erase (8s)”, then, once armed and released, “Keep holding 3 sec to erase”). No prompt implies the same hold both configures and erases. There is **no** GPIO 9 power‑on dependency.

## Wi‑Fi setup portal

The portal is a **temporary, WPA2‑secured SoftAP** at a fixed `192.168.4.1`. It opens automatically on first boot (no stored credentials) and on demand via the configure gesture. It runs for **5 minutes**, then closes; it is **never** a permanent LAN service.

**First-time setup** (no saved Wi‑Fi):

1. The device screen shows the **network name** (`PlaneRadar-XXYYZZ`) and a **one‑time password** — join that Wi‑Fi with the shown password
2. Open **`http://192.168.4.1`** (your phone’s captive‑portal prompt should pop it up automatically)
3. Enter home Wi‑Fi (and optionally location/units/runways), then **Save & test**

**Reconfigure** (device already on your network): hold BOOT **2–8 s** and release to reopen the same secured portal, then change Wi‑Fi/location/units/runways and save.

The submitted credential is **trialed in RAM first**: only after it actually connects — and the driver's active config is re‑verified to still equal the trialed candidate — is it written to flash, so a wrong password never destroys the working network and a stray link event on a stale config is never accepted. On failure the portal reopens with the **same** name/password for the rest of the 5‑minute window. If saved Wi‑Fi later fails to connect the device goes offline and shows the button instructions — it **never** auto‑opens the portal. The one‑time password appears **only** on the panel and is never logged or stored.

The credential transaction is **power‑loss durable**: a tiny dedicated NVS marker (`none` / `commit_in_progress` / `erase_pending`, a versioned + checksummed enum — **never** any password) is persisted and verified before any credential flash is touched and cleared only after the commit or an old‑config rollback is verified. If power is lost mid‑commit, or a boot credential read cannot be verified, the device enters a **fail‑closed credential fault** — it refuses to reconnect or reopen the portal (no false success) until a factory erase recovers it — rather than silently reconnecting on an unknown credential. The Wi‑Fi and hardware RF capture remain the final hardware‑only proof of a good beacon/link; the boot marker just guarantees an interrupted transaction is never mistaken for a normal boot.

**Password field:** leaving it blank reuses the **stored** password **only** when the submitted SSID exactly matches the currently stored network; for any other/new SSID a blank password means an **open** network.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |

Location, units, and runway choices submitted in the portal are staged and applied **only after** the new Wi‑Fi credential is committed, and each is read back to confirm it persisted; if a display setting fails to save the panel shows a truthful "Wi‑Fi saved; settings save failed" warning rather than silently claiming success (a verified Wi‑Fi credential is never rolled back for a display‑setting write failure). A factory erase is guarded by the `erase_pending` marker (so a power loss mid‑erase resumes the erase at the next boot, before any network) and clears Wi‑Fi, location, radar preferences (range, units, runways), and the persisted time‑floor — each step is read back and verified — then restarts only when **every** subsystem verifiably cleared **and** the marker clears. If any step could not be verified it shows a truthful "not fully erased" warning and stays network‑off/fail‑closed; a second confirming erase gesture (or a power‑cycle) retries rather than restarting into a normal boot with settings possibly remaining.

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

### ADS-B network worker (Phase 9, opt-in — not the default)

The **default `supermini` firmware's runtime behavior and mode remain
synchronous and worker-off by default**: ADS-B DNS/TCP/TLS/HTTP fetch work
still runs synchronously in the main loop, exactly as described above. (The
default binary does carry a small shared-code flash delta from this work,
documented in [Memory budget](#memory-budget).) A
separate, explicit opt-in **`supermini-worker`** build (`PLANE_RADAR_ADSB_WORKER=1`)
is a prototype that moves that same fetch work onto one dedicated low-priority
FreeRTOS task, so display refresh, button gesture sampling, and portal pumping
keep running while a fetch is in flight. It issues the **same** ADS-B HTTPS
request and the **same** SNTP time sync as the default build — no new external
traffic or telemetry is added.

**Architecture and security:**

- One long-lived task created with `xTaskCreateStatic` — priority **1**, an
  **8192-byte**, **16-byte-aligned** static stack; no heap task/stack allocation.
- Two `xQueueCreateStatic` queues, each **depth 1** — a strict one-request/
  one-result single-producer/single-consumer handoff between the main loop and
  the worker; no unbounded queuing.
- The worker reuses the **same two** existing `AircraftSnapshot` buffers the
  synchronous path already publishes into — there is **no** third snapshot and
  **no** second framebuffer.
- The main loop keeps sole ownership of display, button, settings, publish,
  provisioning, and NVS state; the worker owns only its own `WiFiClientSecure`
  and writes exclusively into the currently **inactive** candidate snapshot.
- Cancellation is cooperative: a Configure or Erase intent latches and waits
  for the worker to report **Paused** and for its in-flight result to drain
  **before** any radio or NVS mutation proceeds; an Erase intent supersedes a
  pending Configure.
- The default build **compiles the worker path out entirely** — this is
  enforced by a source policy gate; a separately performed release-binary
  `nm` symbol inspection additionally proves the default binary links **zero**
  worker/integration symbols (see [Native tests](#native-tests) and
  [Memory budget](#memory-budget)).

This worker firmware is **for prototype evaluation and hardware validation
only**. It does **not** become the default release firmware until on-device
stack high-water marks, TLS heap peak / largest-free-block behavior, and
long-running concurrency soak tests pass on real hardware — none of which has
happened yet (see [Hardware-only acceptance gates](#hardware-only-acceptance-gates)).

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

The same applies to the opt-in `supermini-worker` prototype (see
[ADS-B network worker](#ads-b-network-worker-phase-9-opt-in--not-the-default)):
its native suites and the `verify-adsb-worker-policy.ps1` source gate prove
protocol/source compile-gating (not the zero-symbol default binary), and a
separately performed release-binary `nm` inspection proves the default binary
links **zero** worker/integration symbols, but the worker's on-device
task stack high-water mark, TLS heap peak / largest-free-block impact while a
fetch runs concurrently with display/button/portal work, and long-running
concurrency behavior are **hardware-only** gates that have **not** been run yet.
A green native/offline run for the worker is not proof of on-device readiness,
and it does **not** make `supermini-worker` the default release firmware.

These gates are now **executable**: an operator-driven checklist, bound to the
exact flashed image SHA-256 and split into **DEFAULT RELEASE** vs **WORKER
PROMOTION**, is defined in `scripts/hardware-acceptance-policy.json` and enforced
by `scripts/verify-hardware-evidence.ps1`. See
[Phase 12 local release pipeline and hardware handoff](#phase-12-local-release-pipeline-and-hardware-handoff)
for the full procedure.

**QEMU was evaluated and deliberately deferred.** There is **no** compatible
Espressif ESP32-C3 QEMU installed here, and QEMU cannot meaningfully prove the
Wi-Fi radio, real TLS-over-Wi-Fi handshakes, or GC9A01 SPI panel behaviour that
these gates cover. There is therefore **no QEMU release gate**; on-device
hardware evidence remains the acceptance path.

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Secure portal | `kPortalIp` (`192.168.4.1`), `kPortalApChannel` / `kPortalApMaxConnections` (1) / `kPortalApVisible`, `kPortalSessionTimeoutMs` (5 min), captive HTTP/DNS `kPortalHttpPort` / `kPortalDnsPort` / `kPortalHttpIdleTimeoutMs` / `kPortalHttpOverallTimeoutMs` / `kPortalHttpWriteDeadlineMs` / `kPortalHttpLimits`, `kCandidateConnectTimeoutMs` (30 s). SSID + secrets are generated at runtime by `core/portal_secrets` — no static AP name, no mDNS |
| Transaction marker | `kProvisionMarkerNvsNamespace` / `kProvisionMarkerNvsKey` — dedicated NVS namespace for the versioned + checksummed power‑loss marker (`core/txn_marker`; `none` / `commit_in_progress` / `erase_pending`, never a password) |
| Wi‑Fi timing | connect attempts (`kWifiConnectAttempts` × `kWifiConnectAttemptMs`), reconnect grace `kWifiDownGraceMs`, retry interval `kWifiReconnectIntervalMs` |
| BOOT / gesture | `kBootPin`, `kButtonPolicy` (approved `core::ProvisionButtonPolicy`: tap/configure/arm/confirm timings, `static_assert`-pinned) |
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
  core/
    adsb_worker_protocol.h  — worker request/result protocol (Phase 9): generations, Paused/fault states
    network_work_intent.h   — Configure/Erase cooperative-cancellation intent shared by main and worker
  services/
    wifi_setup.h            — secure provisioning + runtime link controller (facade)
    config_portal.h         — temporary captive WiFiServer + DNSServer portal transport
    device_identity.h       — factory MAC + esp_fill_random entropy bridge
    wifi_credentials.h      — fixed wifi_config_t snapshot/build/compare + storage
    provision_marker.h      — durable NVS transaction marker (core/txn_marker record)
    radar_location.h
    adsb_client.h
    adsb_worker.h           — opt-in low-priority ADS-B fetch worker (Phase 9; compiled out unless `PLANE_RADAR_ADSB_WORKER=1`)
data/
  ui_font.vlw              — embedded smooth UI font (Noto Sans Bold)
scripts/
  build_large_airports.py
  verify-adsb-worker-policy.ps1  — Phase 9 source policy gate for the opt-in worker (see Native tests)
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  core/
  services/
test/
  native_gfx_support/            — Phase 12 headless render shims, selected ONLY by [env:native-gfx]
    driver/gpio.h                — config.h gpio types/constants (native)
    hardware/lgfx_config.hpp     — shadows the GC9A01 device with a headless LGFX_Sprite canvas
    opencv2/opencv.hpp           — fake header that makes LovyanGFX pick its panel-free desktop backend (no SDL)
  test_native_gfx/               — Unity render suite: headless display/font/adapters, BMP capture+gate, scenes
  golden/                        — checked-in 24-bit BMP golden scenes (exact-byte gate)
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

- PlatformIO env: **`supermini`** (default release build)
- Serial: **115200** baud
- USB CDC on boot enabled in `platformio.ini` for the Super Mini

For a clean Windows build that deletes the project `.pio` directory first:

```powershell
.\scripts\clean-build.ps1
```

**Opt-in ADS-B network worker (`supermini-worker`, prototype — not the default):**
identical to `supermini` except it defines `PLANE_RADAR_ADSB_WORKER=1`, compiling
in the low-priority fetch worker described in
[ADS-B network worker](#ads-b-network-worker-phase-9-opt-in--not-the-default):

```bash
pio run -e supermini-worker
pio run -t merge -e supermini-worker
```

This env is for prototype evaluation and hardware validation only; it is
**not** the default release firmware until the hardware-only gates in
[Hardware-only acceptance gates](#hardware-only-acceptance-gates) pass.

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

The `native` env alone is certified at **580 cases across 39 suites**; `native-diag` runs **5 cases in 1 suite** (`test_runtime_diagnostics_on`); the Phase 12 `native-gfx` headless render gate runs **21 cases in 1 suite** (`test_native_gfx`, one per golden scene); `scripts/native-test.ps1` runs all three for **606 cases across 41 suite runs total**, passing **twice in succession** (no flaky/order-dependent cases). This includes
the Phase 7 trust logic — `test_time_trust` (26 cases: trusted-time state
machine, derived monotonic clock, stale-sample revoke, versioned
persisted-floor record, and the CA-authenticated certificate-`notBefore` floor
ratchet) and `test_cert_time` (17 cases: fail-closed certificate-date parsing,
RFC 5280 validity, CA-signed `notBefore` extraction, and bounded full-chain
peer-certificate validation — expired/future/malformed intermediates, empty and
over-long chains) — the Phase 8 provisioning-hardening suites: `test_http_request`
/ `test_http_router` (HTTP parsing and the closed captive route set),
`test_portal_session` / `test_portal_auth` / `test_portal_secrets` (CSRF and
form-field bounds), `test_url_form` (form decoding), `test_wifi_field` (Wi‑Fi
field validation), `test_provision_button` (the tap/configure/arm/confirm
button FSM), `test_txn_marker` (the power-loss-durable commit transaction),
`test_factory_erase`, and `test_location_record` — plus the four **Phase 9**
opt-in-worker suites added since: `test_adsb_worker_protocol` (worker request/
result generations, Paused transition, and fault propagation),
`test_network_work_intent` (Configure/Erase intent supersedence and quiescence
— Erase always wins over a pending Configure), `test_adsb_fetch_control`
(cooperative fetch cancellation that cancels at stage/refill boundaries and
discards any unpublished partial candidate rather than aborting the transport
mid-byte), and `test_adsb_worker_contract` (the header/pump-action contract
the worker and main loop share) — plus the existing `test_poll_backoff` suite,
extended in Phase 9 (poll-abort handling preserves the transient/permanent
backoff schedule unchanged).

The friend-seam gate, the **offline CA trust gate**, the **provisioning
policy gate**, and the **Phase 9 worker policy gate** are pure PowerShell
(Windows PowerShell 5.1 and pwsh 7), need no ESP32 toolchain, and never touch
the network:

```powershell
.\scripts\check-native-test-access-gate.ps1   # SnapshotStoreTestAccess cannot leak into firmware
.\scripts\verify-ca-bundle.ps1                # 4 pinned roots, no setInsecure, CA enforced, derived-time clock, CA-authenticated cert-notBefore floor
.\scripts\verify-provisioning-policy.ps1      # 8 static release invariants against src/+include/+platformio.ini (no WiFiManager/OTA/mDNS, closed route set, no permanent listener, no secret logging, RAM/flash transaction policy, no insecure AP teardown, one framebuffer, bounded portal)
.\scripts\verify-provisioning-policy.ps1 -SelfTest   # proves the gate itself rejects 9 representative negative tamper cases (re-added WiFiManager, forbidden/extra routes, secret logging, second framebuffer, insecure AP teardown, unguarded storage/teardown, commit-ordering ambiguity), on an isolated temp copy
.\scripts\verify-adsb-worker-policy.ps1              # 10 static source invariants + Phase 10 diag env matrix (17 tamper cases total)
.\scripts\verify-adsb-worker-policy.ps1 -SelfTest    # proves the gate rejects 17 representative negative tamper cases, on an isolated temp copy
.\scripts\verify-diagnostics-policy.ps1              # Phase 10: 12 diagnostics/logging source invariants
.\scripts\verify-diagnostics-policy.ps1 -SelfTest    # proves the diagnostics gate rejects 13 representative tamper cases
.\scripts\verify-reproducible-build-policy.ps1       # Phase 12: firmware envs strip project DWARF (build_unflags=-ggdb + -g0), native envs keep it, policy carries app_descriptor + per-env firmware_elf SHA anchors
```

The worker policy gate proves these invariants hold in **source**; the
release-binary `nm` proof that the default `supermini` build links **zero**
worker/integration symbols is the actual certification step (see
[Memory budget](#memory-budget)).

### Headless render golden gate (Phase 12)

The `native-gfx` environment is a fully local, **offline, headless** rendering
harness that exercises the **actual production UI drawing code** and gates it
against checked-in golden images. It requires **no ESP32 hardware, no SDL2, and
no window** — it runs as part of `scripts/native-test.ps1` (after `native` and
`native-diag`, same pinned w64devkit compiler) and can also be run directly:

```powershell
pio test -e native-gfx
```

**What it compiles.** The real `src/ui/radar_display.cpp`,
`src/ui/runway_overlay.cpp`, and `src/ui/status_screens.cpp`, plus the pure
`core/` and `data/` modules, linked against the same pinned
**LovyanGFX 1.2.25**. No drawing algorithm is duplicated.

**How it renders headless.** LovyanGFX's self-contained, panel-free desktop
backend is selected and its `LGFX` device is shadowed by an in-RAM **240×240
RGB565 sprite canvas** (a `lgfx::LGFX_Sprite`). The production double-buffered
frame path (`LGFX_Sprite s_frame(&tft)` → `pushSprite`) and the status-screen
direct-draw path both run unchanged and composite into that canvas. The exact
repository `data/ui_font.vlw` smooth-font bytes are loaded from disk so text
metrics and anti-aliasing match the firmware's VLW path. All headless shims live
under `test/native_gfx_support/` and `test/test_native_gfx/` and are selected
**only** by `[env:native-gfx]`; the firmware display driver is never changed.

**Scenes (21, one Unity test + one golden BMP each).** Radar: `radar_loading`,
`radar_live_empty`, `radar_live_traffic` (multiple aircraft with
headings/tags/speed vectors, inside-disc and beyond-ring rim behaviour),
`radar_stale` (age badge), `radar_offline` (targets hidden), `radar_nowifi`
(Wi-Fi-disconnected badge), `radar_runways` (runway overlay near the embedded
large airport EHAM), `radar_runways_off` (same location/range/model/inputs as
`radar_runways` but with the runway overlay disabled, making the
enabled/disabled "Show airport runways" contract observable). Status/provisioning: `status_connecting` (saved-network),
`status_portal_preparing`, `status_portal_credentials` (dummy SSID/password/
countdown), `status_candidate_testing`, `status_candidate_failed`,
`status_credential_fault`, `status_button_configure`,
`status_button_confirm_erase`, `status_saved_wifi_failed`,
`status_factory_erase_incomplete`, `status_factory_erase_ok`,
`status_erase_incomplete_persistent`, `status_settings_save_failed`. Scenes pin
all firmware adapters (location, range preset, runway/units toggles, snapshot)
to deterministic literals so ordering never leaks state.

**Golden policy.** Goldens are uncompressed **24-bit BMP** files under
`test/golden/` so any standard image tool can open them. The gate is **exact
byte comparison**. Normal runs are **read-only**: a mismatch writes the actual
image under the ignored `.pio/native-gfx-out/` path and fails without touching
the golden. Goldens are (re)written **only** when `PLANE_RADAR_UPDATE_GOLDENS=1`
is set (an unmistakable opt-in), and every written file is printed. Intentional
updates are **direct-only**: `scripts/native-test.ps1` is **always read-only** —
it fails closed at the top if the environment has `PLANE_RADAR_UPDATE_GOLDENS=1`
and defensively strips the key from the `native-gfx` child, so the certified
gate can never rewrite goldens. To regenerate goldens, run `pio test -e
native-gfx` directly with `PLANE_RADAR_UPDATE_GOLDENS=1`. The suite is
deterministic and byte-identical across repeated runs on the pinned host.

**Fail-closed render preflight.** Before **any** scene comparison or golden
write, the test `main()` verifies the headless canvas is allocated with a live
pixel buffer at exactly **240×240** and that the **exact VLW smooth font** is
loaded (`displayFontIsSmooth()`); `displayInit()` only logs allocation/font
failures, so this preflight prevents update mode from ever blessing
fallback/invalid output. On failure it prints the precise error(s) and exits
nonzero without touching any golden. It is not a separate Unity case.

**What it proves / does not prove.** It proves pixel **geometry, text, layout,
and byte-for-byte deterministic drawing** of the real UI entry points. It is
**not** a colour or panel proof: the BMP stores the raw RGB565 framebuffer
decoded to RGB, so the GC9A01's **BGR channel order, colour inversion, SPI
timing, and brightness are NOT modelled and remain hardware-only**. QEMU remains
**deferred and is not a release gate** — it cannot prove Wi-Fi, TLS-over-Wi-Fi,
or GC9A01 panel behaviour either.

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

**Phase 8 provisioning hardening** (the closed captive route set behind
`http_router`/`http_request`, the CSRF-protected form portal, the power-loss-durable
commit transaction, and the tap/configure/arm/confirm button FSM) is a **net flash
shrink and a small, fixed RAM increase**, measured with the same pinned clean
`supermini` build:

| Build measurement | Phase 7 | Phase 8 | Δ vs Phase 7 |
|-------------------|------:|------:|------:|
| Linker-reported static RAM | 61,788 | 65,044 | +3,256 |
| Linker-reported firmware flash | 1,261,268 | 1,142,914 | −118,354 |
| `firmware.bin` image | 1,328,480 | 1,197,760 | −130,720 |
| `firmware-merged.bin` image | 1,394,016 | 1,263,296 | −130,720 |

Flash shrank because the Arduino `WiFiManager`, `WebServer`, OTA, and mDNS
dependency surface they pulled in was removed in favor of the closed, bounded
captive portal above — a net reduction even after adding the new router/form/CSRF
and transaction-marker code. The +3,256 B static RAM is the bounded, fixed-size
portal and button-controller state (the captive HTTP parser/response workspace,
the button FSM's own state, and the committed/candidate/old Wi‑Fi config
snapshots below) — there is still **no** per-request or per-fetch heap allocation.
The single 240×240 RGB565 frame sprite remains unchanged at **115,200 B** of heap
(still exactly one, heap-allocated once).

Certified on-stack frames (`-fstack-usage`): `wifiBootConnect` 144 B, `wifiLoop`
16 B, `feed` 320 B, `driveController` 208 B, `executeCommit` 208 B,
`executeFactoryErase` 32 B, portal `start`/`pump`/`stop` 64 B / 256 B / 16 B, and
the status screens' worst case 96 B. The largest application frame remains
`drawAircraftFromSnapshot` at 2,048 B (unchanged from Phase 5/6/7). Hardware
stack high-water marks and heap fragmentation remain device-only measurements.

Key static portal buffers: the HTTP response workspace is 2,048 B, the request
body buffer is 1,600 B, and the parser workspace is 1,072 B; the candidate,
old, and expected `wifi_config_t` snapshots used by the commit transaction are
140 B each.

**Phase 9 optional ADS-B network worker** (`adsb_worker`/`adsb_worker_protocol`,
`network_work_intent`, and the `verify-adsb-worker-policy.ps1` gate) leaves the
**default** `supermini` build's runtime mode synchronous and worker-off, with
only a small, fixed flash cost from the shared Phase 9 coordination code and
zero RAM — while the **opt-in** `supermini-worker` build additionally adds the
fixed cost of its static task/stack/queues.
Measured with the same pinned clean build for both envs:

| Build measurement | Phase 8 baseline | Phase 9 default `supermini` | Δ vs Phase 8 | Phase 9 opt-in `supermini-worker` | Δ vs Phase 9 default |
|-------------------|------:|------:|------:|------:|------:|
| Linker-reported static RAM | 65,044 | 65,044 | 0 | 73,924 | +8,880 |
| Linker-reported firmware flash | 1,142,914 | 1,143,328 | +414 | 1,146,194 | +2,866 |
| `firmware.bin` image | 1,197,760 | 1,198,256 | +496 | 1,202,320 | +4,064 |
| `firmware-merged.bin` image | 1,263,296 | 1,263,792 | +496 | 1,267,856 | +4,064 |

The default build's small flash growth (+414/+496 B, 0 RAM) is **not**
compiled-out worker scaffolding — the release binary's symbol table, inspected
with `nm`, shows **zero** worker/integration symbols in the default
`supermini` ELF, so no task/queue/worker-stack code is linked in at all. The
delta is instead the shared Phase 9 coordination/cancellation/completion
refactor (controlled-fetch staging, poll-abort handling, and timekeeper
synchronization) used to preserve synchronous/async equivalence between the
default and worker builds — code that runs in both builds' synchronous path.
The opt-in worker build's **+8,880 B static RAM** is one statically created
task and two statically created depth-1 queues: the fixed **8,192-byte**
worker task stack plus its static TCB, the two depth-1 static queues and their
backing storage, and the small fixed protocol/hook state shared between main
and worker — there is still **no** third `AircraftSnapshot` and **no** second
framebuffer. Its additional flash growth (**+2,866/+4,064 B** vs the Phase 9
default) is the worker adapter, task function, and queue/protocol/integration
code retained only by the opt-in build — not a newly linked
`WiFiClientSecure` instance, since TLS code already exists in the default
synchronous fetch path. On-device
TLS heap peak and stack high-water marks for the worker build remain **unknown**
and are a hardware-only gate (see [Hardware-only acceptance gates](#hardware-only-acceptance-gates)) —
they have not yet been measured on real hardware.

**Phase 10 observability** (`runtime_diagnostics.h`, compile-time logging macros, and
optional fetch/render/heap metrics) adds **zero RAM and near-zero flash overhead to the
default `supermini` build**. The diagnostics instrumentation is fully compiled out unless
`PLANE_RADAR_DIAGNOSTICS=1` is defined; the quiet build (`supermini-quiet`, `LOG_LEVEL=0`)
removes all logging including `Serial.begin`.
Measured with the same pinned clean build for all envs:

| Build measurement | Phase 9 `supermini` | Phase 10 `supermini` | Δ vs Ph 9 | Phase 10 `supermini-quiet` | Δ vs `supermini` | Phase 10 `supermini-diag` | Δ vs `supermini` | Phase 10 `supermini-worker` | Δ vs Ph 9 worker | Phase 10 `supermini-worker-diag` | Δ vs `supermini-worker` |
|------|------:|------:|------:|------:|------:|------:|------:|------:|------:|------:|------:|
| Linker-reported static RAM (bytes) | 65,044 | 65,044 | 0 | 64,916 | −128 | 65,052 | +8 | 73,924 | 0 | 73,948 | +24 |
| Linker-reported firmware flash (bytes) | 1,143,328 | 1,143,278 | −50 | 1,138,158 | −5,120 | 1,143,892 | +614 | 1,146,152 | −42 | 1,146,920 | +768 |
| `.pio/build/*/firmware.bin` (bytes) | 1,198,256 | 1,198,160 | −96 | 1,192,256 | −5,904 | 1,198,880 | +720 | 1,202,224 | −96 | 1,203,120 | +896 |
| `.pio/build/*/firmware-merged.bin` (bytes) | 1,263,792 | 1,263,696 | −96 | 1,257,792 | −5,904 | 1,264,416 | +720 | 1,267,760 | −96 | 1,268,656 | +896 |

Notes (final Phase 10 firmware code at 4dc257d; subsequent commits are documentation-only; confirmed against clean build artifacts):
- **supermini (default)**: −50 B linker flash and −96 B firmware.bin from Phase 9 baseline; 0 B static RAM. Diagnostics are fully compiled out.
- **supermini-quiet** (`LOG_LEVEL=0`): −5,120 B linker flash and −5,904 B firmware.bin (no Serial output, no `Serial.begin`, no logging format strings).
- **supermini-diag** (`DIAGNOSTICS=1`): +8 B static RAM (`g_diag_last_fetch_ms` file-scope state and `s_last_render_diag` struct); +614 B linker flash and +720 B firmware.bin (timing, heap-query, and render-diagnostic code paths).
- **supermini-worker** (opt-in): −42 B linker flash and −96 B firmware.bin from Phase 9 worker baseline.
- **supermini-worker-diag** (`WORKER=1, DIAGNOSTICS=1`): +24 B static RAM (`s_last_render_diag` and diag `fetch_duration_ms` field in `WorkerResultMsg`); +768 B linker flash and +896 B firmware.bin.

### Diagnostics output format (hardware-only interpretation)

When `PLANE_RADAR_DIAGNOSTICS=1`, two additional log lines appear on the serial port:

**After each ADS-B fetch completion** (`diag: fetch_ms=…`):
```
diag: fetch_ms=<ms> outcome=<name> bytes=<n> next_ms=<ms> heap_free=<bytes> heap_min=<bytes> heap_max=<bytes> [worker_hwm=<bytes>]
```
- `fetch_ms`: wall time of `fetchCandidate` (sync path) or `fetchCandidateControlled` on the worker task (worker path, carried through the queue). Measures only the fetch execution; does not include `wifiLoop`/post-processing.
- `outcome`: same name as the INFO completion line.
- `bytes`: decoded response body bytes.
- `next_ms`: scheduled delay until the next fetch (backoff).
- `heap_free`: `ESP.getFreeHeap()` — current free internal heap.
- `heap_min`: `ESP.getMinFreeHeap()` — minimum free internal heap **since boot**.
- `heap_max`: `ESP.getMaxAllocHeap()` — largest single allocatable internal heap block at this snapshot.
- `worker_hwm` (worker build only, when worker started): `workerStackHighWaterBytes()` — worker task minimum free stack since creation. Meaningful only after representative fetch load.

**After each rendered frame** (`diag: render_us=…`):
```
diag: render_us=<us> runway_us=<us> mode=<0-3> age=<s>s runways=<0/1> sprite=<0/1>
```
- `render_us`: wall time of `radarDisplayDraw()` in µs. Includes panel I/O in **both** paths: sprite mode (`sprite=1`) ends with one `pushSprite` SPI transfer; direct-draw mode (`sprite=0`) makes incremental SPI transfers for every drawing call. Hardware-only measurement.
- `runway_us`: wall time of `drawLargeAirportRunways()` in µs. In **sprite mode**, measures off-screen RAM drawing only — panel SPI is **not** included (runways are composited into the off-screen sprite before the final `pushSprite`). In **direct-draw mode**, panel SPI **is** included. Zero when the runway overlay is disabled. Hardware-only.
- `mode`: `RadarDataMode` value (0=Loading, 1=Live, 2=Stale, 3=Offline).
- `age`: freshness age in seconds.
- `runways`: runway overlay setting (`radar::showRunways()`); 1 = overlay on, 0 = off. `drawLargeAirportRunways()` is always invoked but returns early when the overlay is disabled; `runway_us` is zero in that case.
- `sprite`: 1 if the sprite+pushSprite path was used, 0 for direct draw.

**Runway cache decision threshold**: consider caching only after measuring `runway_us > 5000` (> 5 ms) on real hardware **and** `runway_us >= 20%` of total frame time **and** RAM evidence shows a cache cannot threaten the single-frame sprite or TLS heap budget.

**No-secret policy**: no diagnostic line may contain a Wi-Fi credential, provisioning token, CSRF value, SSID, callsign, ICAO hex, host/URL, or body data. This is enforced by the diagnostics policy gate (invariant 4).

### Build variants (Phase 10)

| PlatformIO env | `LOG_LEVEL` | `DIAGNOSTICS` | `WORKER` | Description |
|----------------|------------|---------------|----------|-------------|
| `supermini` (default) | 2 (INFO) | 0 | 0 | Default release firmware; full logging; no metrics |
| `supermini-quiet` | 0 (OFF) | 0 | 0 | No Serial output; no `Serial.begin`; smallest flash |
| `supermini-diag` | 2 (INFO) | 1 | 0 | Full logging + fetch/render/heap metrics |
| `supermini-worker` | 2 (INFO) | 0 | 1 | Opt-in async worker (no metrics) |
| `supermini-worker-diag` | 2 (INFO) | 1 | 1 | Opt-in async worker + metrics |

```bash
pio run -e supermini-quiet          # silent production build
pio run -e supermini-diag           # diagnostics-enabled (fetch/render/heap metrics)
pio run -e supermini-worker-diag    # worker + diagnostics
```

**Log levels** (`include/runtime_diagnostics.h`):
- `0` (`OFF`): all logging compiled out; `Serial.begin` suppressed.
- `1` (`ERROR`): fault/save-failure/fallback messages only.
- `2` (`INFO`): fault + ordinary startup/settings/fetch/range messages (default).

Existing Serial output is replaced by `PLANE_RADAR_LOG_E(fmt, ...)` (ERROR) and
`PLANE_RADAR_LOG_I(fmt, ...)` (INFO) macros. Diagnostic output (Serial.printf
inside `#if PLANE_RADAR_DIAGNOSTICS`) is self-contained and always emitted when
`DIAGNOSTICS=1`, regardless of `LOG_LEVEL`. The provisioning-policy gate
(`verify-provisioning-policy.ps1`) treats `PLANE_RADAR_LOG_E` and
`PLANE_RADAR_LOG_I` as log sinks, so secret-field checks apply to every log call.

### Phase 10 policy gate and native tests

```powershell
.\scripts\verify-diagnostics-policy.ps1          # 12 Phase 10 invariants (source gate)
.\scripts\verify-diagnostics-policy.ps1 -SelfTest # proves the gate rejects 13 representative tamper cases
```

The diagnostics policy gate proves (from source only — **not** ELF):
1. Default/worker env isolation: `supermini` and `supermini-worker` do not define `PLANE_RADAR_DIAGNOSTICS`; diagnostic state and heap APIs are absent from unconditional code.
2. Exact diag/quiet env wiring: `LOG_LEVEL=0` in `supermini-quiet`; `DIAGNOSTICS=1` in `supermini-diag`, `supermini-worker-diag`, `native-diag`; `supermini-diag` has no `ADSB_WORKER`.
3. `runtime_diagnostics.h` defaults: `DIAGNOSTICS=0`, `LOG_LEVEL=2`; level constants OFF=0/ERROR=1/INFO=2; `kDiagnosticsEnabled` and `kLogLevel` constexpr reflection.
4. No raw ungated Serial output: every `Serial.xxx` call in production `.cpp` files is inside a `#if PLANE_RADAR_DIAGNOSTICS` or `#if PLANE_RADAR_LOG_LEVEL` gate. *(Source gate — see ELF proof in Memory budget.)*
5. Provisioning log secret sinks: `PLANE_RADAR_LOG_E` and `PLANE_RADAR_LOG_I` are added to the secret-pattern check.
6. No logging in `adsb_worker.cpp`.
7. Conditional `fetch_duration_ms` field in `WorkerResultMsg`/`WorkerResult` under `#if PLANE_RADAR_DIAGNOSTICS`; `workerTakeResult` copies it.
8. Heap metric APIs: `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()`, `ESP.getMaxAllocHeap()` in a diagnostics-gated block; never `xPortGetFreeHeapSize`.
9. Render/runway instrumentation: `micros()` around `radarDisplayDraw` in `main.cpp`; `micros()` around `drawLargeAirportRunways` in `radar_display.cpp`; `radarDisplayLastDiagnostics()` called from main after the draw (not from within a DrawScope).
10. One framebuffer: still exactly one `LGFX_Sprite` and one `createSprite`.
11. No runway cache: no `std::vector`, `std::array`, `malloc`, `new`, or `s_runway_endpoints` in `runway_overlay.cpp` or `radar_display.cpp`.
12. Diagnostics source gating: `s_last_render_diag` and `RenderDiagnostics` declared only inside `#if PLANE_RADAR_DIAGNOSTICS` blocks.

Two new native test suites are added:
- **`test_runtime_diagnostics`** (runs under `[env:native]`, `DIAGNOSTICS=0`): verifies default values, constexpr reflection, `WorkerResult` trivial copyability without the conditional field (with a C++17 `std::void_t` detection idiom proving field absence), and `elapsedMicros` rollover safety.
- **`test_runtime_diagnostics_on`** (runs under `[env:native-diag]`, `DIAGNOSTICS=1` only): verifies `kDiagnosticsEnabled=true`, that `WorkerResult::fetch_duration_ms` is `uint32_t`, and that `WorkerResult` remains trivially copyable with the added field.

The `native` env runs **580 cases across 39 suites**; `native-diag` runs **5 cases in 1 suite** (`test_runtime_diagnostics_on`). The `native-diag` env uses `test_filter = test_runtime_diagnostics_on` + `test_ignore =` (clearing the inherited exclusion) so the default `native` env and `native-diag` never run each other's macro-sensitive tests. Since Phase 12, `scripts/native-test.ps1` also runs the headless `native-gfx` render gate (**21 cases in 1 suite**), for **606 cases across 41 suite runs total** (see [Native tests](#native-tests)).

The **ELF proof** (`nm` proves symbols; binary-safe scanning proves format strings — both from build artifacts, *not* a source check):
- **Diagnostic symbols**: `g_diag_last_fetch_ms` and `radarDisplayLastDiagnostics` are **absent** from `supermini` and `supermini-worker` ELFs (`nm` confirms) and **present** in `supermini-diag` and `supermini-worker-diag` ELFs. This proves zero diagnostic cost in non-diag builds.
- **Worker task symbols**: `workerTask`, `s_worker_stack`, `s_worker_tcb`, `s_request_q`, `s_result_q`, and `workerCancel` are **absent** from the default `supermini` ELF (`nm` confirms), confirming no worker scaffolding is linked. Worker-variant stack sizes are exactly **0x2000 (8,192) bytes** in `supermini-worker` and `supermini-worker-diag` ELFs.
- **Heap metric APIs**: `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()`, and `ESP.getMaxAllocHeap()` are **absent** from non-diag `supermini` and `supermini-worker` ELFs and **present** in `supermini-diag` and `supermini-worker-diag` ELFs (framework functions `heap_caps_get_free_size` and `heap_caps_get_largest_free_block` exist in all builds and are not isolation indicators); `heap_caps_get_minimum_free_size` presence confirms diag build isolation.
- **Binary-safe format string scan**: `supermini`, `supermini-worker`, and `supermini-quiet` firmware binaries lack the diag format strings `diag: fetch_ms=…` and `diag: render_us=…`, which are present in `supermini-diag` and `supermini-worker-diag` binaries. All shared runtime logging prefixes (time, warnings, font/frame fallback, distance/runway settings, location save/fail/retry, range, ADS-B outcome, startup sequence, render mode) are **absent** from `supermini-quiet` binary (including the exact startup log `Plane Radar\n` — note: substring `Plane Radar` appears in portal HTML and is not an isolation indicator). Presence in default/worker/diag variants confirms selective logging compilation.

The updated **worker policy gate** (`verify-adsb-worker-policy.ps1`) now covers 17 tamper cases (2 new: `supermini-diag` must not add `ADSB_WORKER`; `supermini-worker-diag` must keep `DIAGNOSTICS=1`). All 10 original invariants are preserved.

### Release image (local pipeline)

The default release image is **`supermini/firmware-merged.bin`**, produced inside
a reproducible, re-verifiable release package entirely locally by
`scripts/build-release.ps1`. Source can be hosted in a Git remote, but the
certification policy intentionally requires the independent checkout used for
a release build to have no configured remotes; there is no CI or automated
artifact publication. See
[Phase 12 local release pipeline and hardware handoff](#phase-12-local-release-pipeline-and-hardware-handoff)
for the full pipeline, the manifest/checksum/proof layout, and the hardware
evidence workflow.

Build + merge + proof + package all five envs under `release/<sha>/`:

```powershell
.\scripts\build-release.ps1
```

Low-level PlatformIO merge for a single env (output `.pio/build/supermini/firmware-merged.bin`,
ESP32-C3, 4 MB, flash at **0x0**):

```powershell
pio run -e supermini
pio run -t merge -e supermini
```

Flash + verify the default merged image (put the board in download mode: hold
**BOOT**, tap **RESET**), or use a Web-Serial flasher such as
[esptool-js](https://espressif.github.io/esptool-js/) at offset **0x0**. Use the
**pinned** PlatformIO Python + `esptool.py` and pass the device serial port
explicitly (`<COMx>`, e.g. `COM5`):

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
& $py $esptool --chip esp32c3 --port <COMx> erase_flash
& $py $esptool --chip esp32c3 --port <COMx> --baud 921600 write_flash --flash_mode keep --flash_freq 80m --flash_size 4MB 0x0 release/<sha>/supermini/firmware-merged.bin
& $py $esptool --chip esp32c3 --port <COMx> verify_flash 0x0 release/<sha>/supermini/firmware-merged.bin
```

The former Unix `scripts/merge-firmware.sh` helper (which existed in earlier
phases and wrote a single `release/plane-radar-merged.bin`) has been **retired**
in favour of this Windows/local pipeline.

## Phase 12 local release pipeline and hardware handoff

Everything here runs **locally**: no GitHub Actions and no automated artifact
publication. Release artifacts stay under the git-ignored `release/`
directory. Although the source repository can have an `origin`, certified
release generation must run from a separate, clean repository copy with all
Git remotes removed; the fail-closed release policy verifies that condition.

### Build a release (`scripts/build-release.ps1`)

```powershell
.\scripts\build-release.ps1
```

Fail-closed: the build aborts **before** building or publishing unless PlatformIO
is exactly **6.1.19**; git `HEAD` exists with a clean tracked/index worktree, **no
untracked files**, and **no git remote**; the publish path is always exactly
`release/<full-git-sha>/` (there is **no** output-root override); and every required
source gate is green — `scripts/native-test.ps1` (**606/41**, which also runs the
airport + egress gates), plus `check-native-test-access`, CA, provisioning, worker,
diagnostics, and **reproducible-build** policy gates. The certification policy is the **tracked git-blob**
`release-policy.json` at `HEAD` (not the working tree); its SHA-256 is recorded in
the manifest as `policy_sha256`. There are **no certification bypasses** (no
`-SkipSourceGates`/`-SkipBuild`/`-AllowDirtyWorktree`/`-PolicyPath`): every
published package is **certified**. Iterate with direct `pio run` commands instead.

It then deletes `.pio` once (reparse-point-safe) and freshly builds **and merges**
exactly the five firmware envs (`supermini`, `supermini-worker`, `supermini-quiet`,
`supermini-diag`, `supermini-worker-diag`), enforces the **exact** approved
resource/file sizes **and the exact policy SHA-256** of each env's `firmware.elf`,
`firmware.bin`, and shipped `firmware-merged.bin` (plus the `firmware.bin` ↔
`firmware.elf` embedded-SHA **elf-binding**; see
[Build variants](#build-variants-phase-10)
and [Memory budget](#memory-budget)), **verifies and records the OBSERVED installed
toolchain/library versions** (PlatformIO platform, `framework-arduinoespressif32`,
RISC-V toolchain, `esptool`, LovyanGFX, ArduinoJson) and fails on any drift from the
pins, runs the current-head ELF/binary proofs (symbol/logging, the re-derived
merged-layout invariants, and the elf-binding + path-independence invariants), and
only if **every** invariant passes stages
and publishes the package to `release/<full-git-sha>/`. **Immediately before
staging** it re-runs the Git source-gate and requires the same commit/branch, a
clean tracked worktree, zero untracked files, and no remote (closing the
source-gate/build TOCTOU); a failed stage/publish cleans only its exact validated
`.stage-*` directory. It refuses to overwrite an existing release unless `-Force`
(which replaces only that exact validated path, reparse-point-safe).

Output tree:

```
release/<full-git-sha>/
  manifest.json          schema/commit/branch/UTC, local-only state, tracked-policy
                         SHA-256, PlatformIO/platform/framework/toolchain/dependency
                         pins PLUS the OBSERVED installed versions, airport source
                         commit, per-env options + RAM/flash/file sizes, SHA-256 of
                         every file, certification + proof + gate/test summary, and
                         the default-artifact identity (worker images are eval-only)
  CHECKSUMS.sha256       sorted "<sha256>  <path>" over manifest + all files
                         (except CHECKSUMS itself)
  binary-proof.json      machine-readable proof (every invariant, pass/fail)
  binary-proof.txt       human-readable proof
  supermini/             firmware.bin, firmware-merged.bin, firmware.elf,
                         firmware.map, build.log, merge.log, nm-symbols.txt
  supermini-worker/      (evaluation-only)
  supermini-quiet/       (evaluation-only)
  supermini-diag/        (evaluation-only)
  supermini-worker-diag/ (evaluation-only)
```

The **only** default release image is **`supermini/firmware-merged.bin`**; the
worker images are clearly marked **evaluation-only**. The **generated text
metadata/logs** (`manifest.json`, `CHECKSUMS.sha256`, `binary-proof.json`/`.txt`,
`build.log`, `merge.log`, `nm-symbols.txt`) are written **LF / UTF-8 without BOM**;
the firmware `*.bin`/`*.elf`/`*.map` artifacts remain **binary** (byte-exact, never
line-ending normalized). This package binds the **exact current-head binaries
only** — it does **not** claim raw byte equivalence to any Phase 10/11 artifact.

### Current-head ELF/binary proofs

The per-environment proof spec is defined **once** in `scripts/release-common.ps1`
(`Get-ProofSpec` / `Get-BinaryProofInvariants` / `Get-MergedLayoutInvariants`) and
is re-derived **byte-for-byte identically** by `build-release` (at build time) and
`verify-release` (from the packaged binaries). Using the pinned PlatformIO RISC-V
`nm` and binary-safe byte searching, the build proves and records (pass/fail) for
every env, aborting before publishing on any failure:

- the default `supermini` links **zero** worker/integration symbols (`workerTask`,
  `s_worker_stack`, `s_worker_tcb`, `s_request_q`, `s_result_q`, `workerCancel`),
  matched as whole demangled tokens (so `core::workerCancelRequested` is not a
  false match);
- `supermini-worker` and `supermini-worker-diag` contain exactly one worker stack
  symbol of exactly **0x2000 (8192) bytes**;
- diagnostic symbols (`g_diag_last_fetch_ms`, `radarDisplayLastDiagnostics`) and
  the heap-diagnostic API (`ESP.getFreeHeap`/`getMinFreeHeap`/`getMaxAllocHeap`,
  plus `heap_caps_get_minimum_free_size`) are **absent** from non-diag ELFs and
  **present** in the diag ELFs — while the shared `heap_caps_get_free_size` /
  `heap_caps_get_largest_free_block` (present in every build) are explicitly **not**
  used as isolation indicators;
- the diagnostic binary strings `diag: fetch_ms=` / `diag: render_us=` are absent
  from non-diag firmware binaries and present in both diag binaries;
- the quiet binary lacks the Serial logging strings and the **exact** startup log
  `Plane Radar\n`, while the non-quiet builds contain them (plain `Plane Radar`
  appears in every build's portal HTML and is deliberately not used as an
  indicator).

**Exact-SHA anchoring of the shipped image + its proof ELF (Phase 12 final).**
Self-consistent manifest hashes are *not* an anchor, so the packaged
**`firmware.elf`** (the input the verifier re-derives the nm proofs from), the
shipped/flashed **`firmware-merged.bin`**, and **`firmware.bin`** of every env are
all bound to **tracked policy**. `release-policy.json` records the exact approved
SHA-256 of each env's `firmware.elf`, `firmware.bin`, and `firmware-merged.bin`
(`environments[].sha256`) plus its `firmware_elf` size, a `merged_layout` block,
and an `app_descriptor` block; the build **and** verify re-derive **directly from
the packaged bytes** (never `merge.log` text):

- each merged image's component regions — **bootloader @ 0x0**, **partitions @ 0x8000**,
  **boot_app0 @ 0xe000** — match their fixed offset/size and exact SHA-256 (identical
  across all five envs);
- the region at **app_offset (0x10000)** is **byte-for-byte** equal to that env's
  `firmware.bin`, and the merged length equals `0x10000 + firmware.bin length`;
- every gap between a component end and the next offset is entirely **0xFF**;
- the packaged `firmware.elf`/`firmware.bin`/`firmware-merged.bin` equal the exact
  policy SHA-256 anchors;
- **`elf-binding`** — `firmware.bin`'s ESP app-descriptor embedded ELF SHA-256
  (at `app_descriptor.elf_sha256_offset` = **0xB0**, provenance tracked in policy)
  equals SHA-256(packaged `firmware.elf`). Because `firmware.bin` is pinned
  byte-exact, this **binds the proof-input ELF to the app image**: a package
  attacker who swaps `firmware.elf` and reseals `manifest.json`/`CHECKSUMS` while
  leaving the pinned `firmware.bin` untouched is rejected (its new digest no
  longer matches the embedded bytes *or* the policy ELF anchor);
- **`path-independence`** — the packaged `firmware.elf` embeds **no** absolute
  build-worktree path (neither the repo-root path in any Windows drive-case/slash
  spelling nor the `esp32-plane-radar` project-family token), proving the build
  did not leak its own path into DWARF.

A same-length all-zero (or app-/bootloader-region-swapped) merged image, or a
swapped/byte-flipped `firmware.elf`, or a tampered embedded ELF-SHA — even with
the manifest, nested artifact maps, `default_artifact`, and `CHECKSUMS` all
consistently updated — is therefore **rejected** on the exact policy SHA and/or
the re-derived merged-layout / elf-binding / path-independence invariants. A
compact `nm-symbols.txt` (the relevant demangled symbols) is saved per env for
later audit.

### Byte-reproducible builds across worktree paths (Phase 12 final)

The exact raw SHA-256 anchors above are only meaningful if the same commit +
toolchain produces byte-identical artifacts regardless of the absolute build
path. The Arduino framework compiles every firmware TU with `-ggdb`, which embeds
the absolute worktree path into DWARF (`DW_AT_comp_dir` in `.debug_str`, plus an
absolute directory entry in `.debug_line`). Those bytes are the **only** thing
that differs between two worktrees, but they change `firmware.elf`, which cascades
through the esptool-embedded app-descriptor ELF SHA-256 into `firmware.bin`
(exactly 65 identity/integrity bytes: the 32-byte ELF SHA at 0xB0-0xCF plus the
1-byte image checksum + 32-byte validation hash near the image end).

`platformio.ini` therefore strips project DWARF from the five firmware envs with
**`build_unflags = -ggdb`** + **`-g0`** (propagated to every `supermini-*` env via
`extends` + `${env:supermini.build_flags}`; the `native*` test envs keep their
debug info). `-fdebug-prefix-map`/`-ffile-prefix-map` were tested first but are
unreliable on Windows/MinGW (GCC records `comp_dir` with the getcwd() spelling yet
canonicalises other absolute paths to a lower-cased/forward-slashed spelling, so
no single prefix-map covers every spelling). Stripping DWARF is safe because the
binary proofs read the ELF **symbol table** (`nm .symtab`) and loadable image
strings, **not** DWARF — every worker/diag/heap symbol and logging-string proof is
retained; only `.debug_*` from project TUs is removed and the **loadable image is
byte-identical** (only the embedded ELF SHA and its dependent checksum/hash
change). Espressif's precompiled SDK archives keep their own stable
(worktree-independent) debug paths, which do not vary across worktrees.

This was **verified by building the final commit in two worktrees with materially
different absolute path lengths** (`D:\repos\esp32-plane-radar-lambert-phase12-release`,
50 chars, and a temporary `D:\plr-repro-worktree2`, 22 chars) and comparing all 15
artifacts (5 envs × `firmware.elf`/`firmware.bin`/`firmware-merged.bin`) —
**byte-identical SHA-256 in every case**. The `verify-reproducible-build-policy`
source gate enforces the `platformio.ini` flag policy + the policy ELF anchors,
and the `path-independence` proof invariant enforces the built output. To
reproduce:

```powershell
git worktree add --detach D:\plr-repro-worktree2 <final-sha>
Copy-Item .\platformio.ini D:\plr-repro-worktree2\   # if verifying pre-commit
pushd D:\plr-repro-worktree2; pio run -e supermini; pio run -t merge -e supermini; popd
# compare .pio\build\supermini\firmware.{elf,bin} and firmware-merged.bin SHA-256
git worktree remove --force D:\plr-repro-worktree2
```

### Verify a release (`scripts/verify-release.ps1`)

Re-verifies an existing package **without rebuilding**:

```powershell
.\scripts\verify-release.ps1 -Path release/<sha>
.\scripts\verify-release.ps1 -SelfTest        # isolated tamper self-test
```

It **rejects by default** unless the package is a fully **certified**, clean,
local-only, gate-green build whose manifest / CHECKSUMS / proof are mutually
consistent. The **certification policy is bound to Git**: `release-policy.json`
(and, for the hardware handoff, `hardware-acceptance-policy.json`) is loaded from
the **exact commit blob** the package was built at (`git show <commit>:…`), never
the editable working tree, and the manifest records a `policy_sha256` that must
equal the SHA-256 of those exact git-blob bytes — a dirty/attacker-modified
working-tree policy cannot influence certification, and there is **no**
`-PolicyPath` override. It checks: safe path under `release/` (and no
symlink/junction/reparse point on the release root or anywhere in the package
subtree); manifest↔directory commit binding (and, by default, that the commit
matches the current `HEAD` — `-AllowStaleHead` validates an archived but
still-**certified** local package, using the policy **at the package commit**);
`build.certified` + `clean_build`; `git.tracked_clean`, zero untracked files, and
`local_only.no_remote`; the source gates are the **exact** duplicate-free policy
set and every status is `passed` (with `gates_run` and a policy-consistent
native-test summary); **every** declared pin (core + verified banner string /
platform / framework / framework package / toolchain / esptool / **board** /
**mcu** / **flash_size** / **app_offset** / dependencies) **and the OBSERVED
installed toolchain/library versions** both equal policy; every manifest artifact
size + SHA-256 **and env ownership**; the **exact policy SHA-256** of each env's
`firmware.elf`, `firmware.bin`, and shipped `firmware-merged.bin`, plus the
`firmware.bin` ↔ `firmware.elf` embedded-SHA **elf-binding**; the artifact keys
are **exactly**
the per-env copied files + `binary-proof.json`/`.txt` (no unlisted extras /
missing); every `CHECKSUMS` line recomputed with no missing/extra/duplicate and no
traversal/absolute/ADS/unsafe path; the environments are the **exact duplicate-free**
policy set with matching role/eval/options/resources, and each env's **nested
artifact map** is an exact copy of the flat map (a forged nested hash is rejected);
the default-artifact identity/role/size/hash; and the binary proof **re-derived
from the packaged `firmware.elf`/`firmware.bin`/`firmware-merged.bin` with the
pinned `nm`** (including the merged-layout, elf-binding, and path-independence
invariants) — every declared invariant,
total, category, and the manifest `proof_summary` must match the re-derivation
(self-declared status is never trusted; there is no skip-recompute switch).
`-SelfTest` builds a synthetic package in an isolated temp fixture (whose merged
image has a real component/gap/app layout and a real embedded-ELF-SHA binding) and
proves the verifier accepts a
well-formed package and rejects a **45-case** tamper matrix: artifact byte,
manifest commit, checksum corruption, `certified=false`, `clean_build=false`,
dirty/untracked worktree, configured remote, skipped/missing/duplicate gate,
`gates_run=false`, inconsistent native-test summary, expected/observed pin
mismatch, env-option mismatch, default-size mismatch, unlisted/wired extra file,
missing artifact file, forged/trimmed/contradictory proof,
traversal/duplicate/missing/ADS checksum entries, **same-length all-zero merged**,
**app-region and bootloader-region merged mismatches** (each fully re-sealed),
wrong **board/mcu/app_offset/framework** pins, **duplicate environment**, **forged
nested artifact hash**, wrong flat-artifact **owner**, tampered
**`policy_sha256`**, a **reparse-point (junction)** planted inside the package, and
— Phase 12 final — a **swapped/byte-flipped `firmware.elf`** and an **embedded
ELF-SHA byte tamper** (each fully re-sealed with `firmware.bin`/merged left
untouched), rejected on the ELF policy anchor / `elf-binding` / re-derived proof.
Works under Windows PowerShell 5.1 and pwsh 7.

### Executable hardware handoff

`scripts/hardware-acceptance-policy.json` is a strict, executable checklist split
into **DEFAULT RELEASE** (items 1–8, 10–12) and a separate **WORKER PROMOTION**
gate (item 9, a 72 h `supermini-worker-diag` soak). Each item has exact commands
(flash/erase/verify via the **pinned** PlatformIO Python + `esptool.py` with an
explicit `--port <COMx>`), numeric pass/fail thresholds, and exact evidence
filenames, and binds evidence to the flashed image SHA.

```powershell
# Create a PENDING evidence template bound to the exact flashed default image:
.\scripts\initialize-hardware-evidence.ps1 -Path release/<sha>

# After an operator completes real-hardware evidence, verify a gate:
.\scripts\verify-hardware-evidence.ps1 -Path release/<sha>                       # default release
.\scripts\verify-hardware-evidence.ps1 -Path release/<sha> -Gate worker-promotion
.\scripts\verify-hardware-evidence.ps1 -SelfTest                                 # isolated self-test (24 cases)
```

`initialize-hardware-evidence.ps1` first re-verifies the (certified) release, then
writes `release/<sha>/hardware-evidence/hardware-results.json` (every item
`pending`, with thresholds, expected evidence filenames each scaffolded
`size=null`/`sha256=""`, and operator fields) plus the empty evidence
subdirectories — it **never** fabricates passing evidence, and the evidence lives
under the ignored release package (never committed). Both scripts load the
`hardware-acceptance-policy.json` (and `release-policy.json`) from the **tracked git
commit blob** of the package — there is **no** `-HardwarePolicyPath`/`-PolicyPath`
override — and bind the exact hardware-policy SHA-256 into the results
(`policy_sha256`), which the verifier re-checks; they also reject a
symlink/junction/reparse point on the release root, the evidence root, any
ancestor, or anywhere under the evidence tree.
`verify-hardware-evidence.ps1` re-verifies the release, validates the authoritative
policy `schema`/`schema_version` and the results
`schema`/`schema_version`/`policy_ref`/`policy_sha256`, requires the item set to be
**exactly** the policy items (no missing/extra/duplicate), binds the top-level
**and each item** to their exact flashed image path/size/SHA-256/gate/order/
environment, requires each item's thresholds and evidence names to match policy
exactly (the operator may edit only `measured`), and — for every mandatory item —
requires `status=pass`, a non-empty operator name + a **real** ISO calendar date
(`yyyy-MM-dd` parsed invariantly, so `2026-99-99`/`2026-02-30` are rejected), and
each evidence file present, non-empty, with its recorded `size` + lowercase
`sha256` **re-hashed and matched** (cryptographic binding; the old editable
`present` flag is gone). It fails on `pending`/`blocked`/`waived`, only **reports**
worker-promotion readiness, and never promotes the worker; the default artifact
always remains `supermini`. The self-test exercises 24 cases (wrong image
SHA/size/path/environment, duplicate/extra item, forged evidence hash/size, missing
operator, non-ISO **and impossible-calendar** dates, ADS/traversal evidence path,
pending status, threshold + bounded-`between` failures, weakened/missing threshold,
empty/missing evidence, forged `policy_sha256`).

Item 6 measures memory/performance on the `supermini-diag` build of the **same
commit** (the default `supermini` emits no heap metrics), including the long-term
(≥ 6 h) heap-trend / steady-state drift (≤ 4096 B on like-for-like post-fetch
samples); the released default remains `supermini`. The separate 48 h default-image
soak (item 8) covers resets/recovery and the **bounded** stale (15–17 s) / offline
(60–62 s) transitions. Dense airspace (item 7) uses a **concurrent operator-side
HTTPS query** to the exact `/api/v3/lat/%.6f/lon/%.6f/dist/%.1f` provider request to
observe ≥ 65 source objects (the device retained count is capped at 64). Thresholds
are conservative and justified in the policy from the ESP32-C3 one-framebuffer
design (240×240 RGB565 sprite ≈ 115 KB against the 327,680-byte / 320 KiB
application RAM budget, ~23 ms SPI transfer floor at 40 MHz) rather than any
measured claim.

### Headless goldens vs hardware-only panel proof

The [headless render golden gate](#headless-render-golden-gate-phase-12) proves
pixel **geometry, text, layout, and byte-for-byte deterministic drawing** of the
real UI code. It is **not** a colour/panel proof: the GC9A01's BGR channel order,
colour inversion, SPI timing, and brightness are decoded away in the BMP and remain
**hardware-only** (item 10). **QEMU** was evaluated and **deferred** — no compatible
Espressif ESP32-C3 QEMU is installed, it cannot meaningfully prove Wi-Fi/TLS/GC9A01
behaviour, and there is **no QEMU release gate**.

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

The secure setup portal uses only the Arduino‑ESP32 built‑in Wi‑Fi stack — an Arduino **`WiFiServer`** plus **`DNSServer`** — inside a temporary SoftAP session. The `WiFiServer` is constructed with the exact‑IP constructor `WiFiServer(IPAddress(192,168,4,1), 80, 1)`, so it binds **only** the SoftAP address `192.168.4.1` with a one‑client backlog (never `INADDR_ANY`); the portal treats it as listening only after `begin()` and `operator bool()` are both true, accepts through `accept()` (never `available()`), and tears down with `end()`. To keep request secrets out of `WiFiClient`'s internal 1436‑byte RxBuffer, the accepted client's bytes are pumped with non‑blocking `lwip_recv`/`lwip_send` on `WiFiClient::fd()` and each raw read chunk is wiped after it is parsed. The SoftAP itself is brought up via a controlled paired low‑level sequence (`esp_wifi_stop` → `esp_wifi_set_mode(AP)` → `esp_wifi_set_config(AP, WPA2‑PSK/CCMP + one‑time secret)` → `esp_wifi_start`) so the **first** joinable beacon already carries the secret — never an open/default beacon. There is **no** WiFiManager, `WebServer`, mDNS, or OTA dependency.

## Runtime egress, privacy, and portal network behavior (Phase 11)

### Runtime network egress

| Purpose | Protocol / port | Fixed application/service destination |
|---------|-----------------|----------------|
| ADS-B live data | **HTTPS — TCP/443** | `opendata.adsb.fi` (Cloudflare-fronted) |
| Time synchronization | **SNTP — UDP/123** | `time.cloudflare.com` |

`opendata.adsb.fi` and `time.cloudflare.com` are the only fixed application/service destinations selected by the firmware. `time.cloudflare.com` is the sole default SNTP server. Up to two optional compile-time SNTP fallback servers may be configured (`kSntpServerFallback1/2` in `config.h`), but these are **empty by default** and **policy-gated** — the egress policy gate (`scripts/verify-egress-policy.ps1`) fails if they are non-empty. **DHCP-provided NTP is explicitly disabled** (`esp_sntp_servermode_dhcp(false)`).

Ordinary **DNS/DHCP infrastructure** traffic is also needed to join the configured network and resolve those hostnames. The firmware does not select a fixed third-party DNS operator: DNS and DHCP servers are supplied by the local network, and their operators can vary by network.

**Build-time OurAirports and GitHub traffic** (downloading `airports.csv`/`runways.csv` from the pinned immutable commit URL during regeneration) is a **build/development-time activity only** and is not firmware runtime egress. Regeneration runs on a developer machine, not on the ESP32 device.

### Privacy

The firmware sends the following data to external parties at runtime:

| Data sent | Recipient | Detail |
|-----------|-----------|--------|
| ADS-B query parameters | `opendata.adsb.fi` | Lat/lon formatted/rounded to six decimal places (`%.6f`), radius converted to nautical miles formatted to 0.1 NM (`%.1f`) in the URL path |
| Normal TCP/IP source IP | `opendata.adsb.fi`, `time.cloudflare.com` | Network-layer source-address metadata: local/private on the LAN and public after NAT where applicable |
| SNTP client packets | `time.cloudflare.com` | Standard NTP exchange; no device-specific payload |

**The firmware does not send:**
- Wi-Fi credentials, provisioning secrets, CSRF tokens, or SSID to any external service
- Device MAC address or any hardware identifier to any external service
- Aircraft callsigns, ICAO hex addresses, or ADS-B payload copies to any external service
- Analytics, heartbeat, error reporting, or telemetry of any kind to any external service

**Local network note:** Normal Wi-Fi and DHCP link-layer operation exposes the device MAC address to the **local network** (access point and devices on the same LAN segment). This is standard 802.11 behavior and is not specific to this firmware.

The ADS-B provider and DNS/SNTP services can observe the device's source address (including a **public IP address** after NAT where applicable), but each party observes only what its own protocol carries:

- The **ADS-B provider** (`opendata.adsb.fi`) sees the source address plus the lat/lon and radius request path (the `%.6f`/`%.1f` values in the HTTPS URL). It does **not** perform the DNS or SNTP exchanges.
- The **DNS resolver** sees the source address plus the queried hostnames (`opendata.adsb.fi`, `time.cloudflare.com`). Because the ADS-B request travels over HTTPS, the DNS resolver does **not** see the HTTPS URL path or the lat/lon/radius query parameters.
- The **SNTP service** (`time.cloudflare.com`) sees the source address plus the standard NTP time exchange. It does **not** see any HTTPS URL path or query parameters.

The lat/lon values are formatted/rounded to six decimal places and the radius to 0.1 NM; this formatting is **not** a privacy-preserving precision reduction and is not necessarily the same precision as the configured values.

### Portal network behavior

The setup portal creates a **temporary, session-scoped SoftAP** at `192.168.4.1`:

- **WPA2-PSK** with a newly random password for each provisioning session, displayed on-device; never open. The SSID, not the password, is MAC-derived.
- **Session-scoped**: exists only while a setup session is active (max 5 minutes)
- **No permanent listener**: the portal is torn down after the session ends
- **Wildcard DNS** at `192.168.4.1` (port 53) redirects all DNS queries to the captive portal for the duration of the session only
- **HTTP at port 80** at `192.168.4.1` only (bound to the exact SoftAP IP, not `INADDR_ANY`)
- **No mDNS, no OTA, no permanent LAN listener** after the session closes

After setup completes the device connects to the configured home Wi-Fi as a normal station. The SoftAP and DNS server are fully torn down.

### Source gate vs hardware packet-capture

`scripts/verify-egress-policy.ps1` is a **source-only** gate. It proves from source code that:
- Only the approved host constants are used for runtime connections
- The DNS/TLS/HTTP Host header derives from `config::kAdsbHost`
- DHCP NTP is disabled, empty fallbacks become `nullptr`
- No banned clients (HTTPClient, WiFiUDP, mDNS, OTA, WiFiManager) appear in production source

**The source gate is necessary but not sufficient.** A hardware **packet capture** on a physical device (e.g., via a Wi-Fi monitor-mode sniffer or router firewall log) is the final runtime proof that the firmware's actual network traffic matches this policy.

### OurAirports data provenance (Phase 11)

The embedded large-airport runway overlay data is generated from a **pinned, immutable OurAirports commit**:

| Property | Value |
|----------|-------|
| Repository | `https://github.com/davidmegginson/ourairports-data` |
| Commit | `79efa72ec1e344d91b081160634fa042a56a21b8` (2026-06-08T01:53:13Z) |
| `airports.csv` SHA-256 | `092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354` |
| `airports.csv` length | 12,651,071 bytes |
| `runways.csv` SHA-256 | `312f9ded8a5a29f8634bd615b0a7aadd4ed01e773ae63e5aab7c510629440fec` |
| `runways.csv` length | 3,951,490 bytes |
| License | Public Domain / The Unlicense |
| Filter | `type=large_airport`, 4-char ICAO ident, open runways, helipads excluded, endpoint coordinates required |
| Filter schema version | 1 |
| Result | **1,166 airports**, **1,706 runways** |

**Regeneration (online, pinned URL):**
```bash
python3 scripts/build_large_airports.py
```
Downloads from the immutable pinned commit URL; verifies SHA-256 and byte length before parsing; writes deterministic LF bytes. No `/main/` URL is used.

**Regeneration (offline, local CSV files):**
```bash
python3 scripts/build_large_airports.py \
  --airports-csv path/to/airports.csv \
  --runways-csv  path/to/runways.csv
```
Both flags must be specified together (both or neither). The local files must match the pinned SHA-256 and byte length.

**Verify checked-in files match (offline check mode):**
```bash
python3 scripts/build_large_airports.py \
  --airports-csv path/to/airports.csv \
  --runways-csv  path/to/runways.csv \
  --check
```
Renders in memory and compares exact LF bytes to checked-in files. Exits 0 if identical, non-zero otherwise. Never modifies files.

**Update workflow:** to update to a new OurAirports commit, update `_COMMIT`, `AIRPORTS_SHA256`, `RUNWAYS_SHA256`, `AIRPORTS_LENGTH`, `RUNWAYS_LENGTH`, `AIRPORTS_BLOB`, `RUNWAYS_BLOB`, and `_COMMIT_DATE` in `scripts/build_large_airports.py`, regenerate both files, and update the provenance table above.

**Generated files:** `include/data/large_airports.h` and `src/data/large_airports_data.cpp` are committed checked-in with LF line endings (enforced by `.gitattributes`). They contain a stable provenance comment block (commit/SHA-256/lengths/blob SHAs/license URL/filter schema version) and must not be edited manually.

### Local release policy

`.github/workflows/` is **intentionally absent** — there are no CI/CD workflows
or automated releases. Source hosting is separate from certification: all
verification, gate, and release operations run locally:

```powershell
.\scripts\verify-airport-data.ps1              # Phase 11: OurAirports provenance gate (20 invariants)
.\scripts\verify-airport-data.ps1 -SelfTest    # proves the gate rejects 25 tamper cases
.\scripts\verify-egress-policy.ps1             # Phase 11: runtime egress source policy gate (12 invariants)
.\scripts\verify-egress-policy.ps1 -SelfTest   # proves the gate rejects 36 tamper cases
```

The `scripts/native-test.ps1` script runs both Phase 11 live gates (`verify-airport-data.ps1` and `verify-egress-policy.ps1`) fail-fast in its current PowerShell process before any PlatformIO tests; it does not run gate self-tests.

The configured source remote is not used by these scripts. A certified release
must be generated from an independent clean checkout with no configured Git
remotes; release artifacts remain local unless a maintainer deliberately
publishes them, and no GitHub Actions workflows are introduced by this project.

### Phase 11 native tests and gate summary

The Phase 11 native test suite `test_large_airport_data` adds **13 cases** in 1 suite to the native test run:

- `test_airport_count_matches_constant` / `test_runway_count_matches_constant`: runtime count equals compile-time constant (1166/1706)
- `test_airport_idents_are_4_chars` / `test_airport_idents_unique` / `test_airport_idents_sorted`: ident validity
- `test_airport_lat_range` / `test_airport_lon_range`: coordinate bounds
- `test_runway_endpoint_coordinate_ranges` / `test_runway_lengths_positive` / `test_runway_index_bounds` / `test_runway_ordering`: runway structural integrity
- `test_compile_time_extent_airport` / `test_compile_time_extent_runway`: `std::extent` matches constants

With the pinned local toolchain, `scripts/native-test.ps1` passes **580 cases across 39 suites** in `native`, **5 cases in 1 suite** in `native-diag`, and (since Phase 12) **21 cases in 1 suite** in the headless `native-gfx` render gate, for **606 cases across 41 suite runs total**.

New Phase 11 gate commands:

```powershell
.\scripts\verify-airport-data.ps1              # 20 source invariants
.\scripts\verify-airport-data.ps1 -SelfTest    # 25 tamper cases
.\scripts\verify-egress-policy.ps1             # 12 source invariants
.\scripts\verify-egress-policy.ps1 -SelfTest   # 36 tamper cases
```
