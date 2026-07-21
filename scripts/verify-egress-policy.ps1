[CmdletBinding()]
param(
  [string]$ProjectRoot,
  [switch]$SelfTest
)

# ===========================================================================
# Phase 11 runtime egress source policy gate.
#
# Deterministic, OFFLINE, source-only verification that the runtime firmware
# egress policy holds in PRODUCTION SOURCE (src/ + include/ + platformio.ini).
#
# This gate distinguishes three traffic classes:
#   RUNTIME: firmware connections made by the ESP32 device at runtime.
#   BUILD-TIME: Python/PowerShell scripts downloading provenance CSVs/certs.
#   LOCAL PORTAL: inbound WiFiServer/DNSServer captive portal at 192.168.4.1.
#
# This gate concerns RUNTIME egress only. Build-time OurAirports/GitHub URLs
# in scripts/ are not firmware runtime egress and must not create false
# positives here. PEM certificate data in source must not create false
# positives either.
#
# Invariants proved from source alone (12 total):
#   1.  Exact approved host/port constants: kAdsbHost="opendata.adsb.fi",
#       kAdsbPort=443, kSntpServerPrimary="time.cloudflare.com" in config.h.
#   2.  Empty fallback SNTP constants: kSntpServerFallback1="" and
#       kSntpServerFallback2="" in config.h.
#   3.  Approved DNS/TLS/HTTP Host chain: config host/port flow from the ADS-B
#       call site through espTlsConnect; the actual std::snprintf(http_request,
#       ...) statement is bounded and its final Host %s argument must be
#       config::kAdsbHost; WiFi.hostByName(host,address) and
#       client.connect(address,port,host,ca_bundle,...) are scoped to the actual
#       espTlsConnect function body (executable checks run on a strings- and
#       comments-blanked skeleton so string literals cannot satisfy them).
#   4.  Full SNTP server chain as executable code: s1=kSntpServerPrimary,
#       s2/s3 = kSntpServerFallbackN[0] != '\0' ? kSntpServerFallbackN : nullptr,
#       configTime(0,0,s1,s2,s3). Alternate SNTP paths (configTzTime,
#       sntp_setservername/esp_sntp_setservername) are banned; the time-sync
#       notification callback stays allowed. esp_sntp_servermode_dhcp(false) set.
#   5.  Exactly one production DNS and outbound client-connect executable path
#       (counted on the blanked skeleton); no HTTPClient, WiFiUDP, raw outbound
#       socket, alternate client (esp_http_client, esp_tls_conn*, getaddrinfo,
#       dns_gethostbyname, tcp_connect, udp_connect, AsyncUDP, NetworkClient/
#       NetworkUDP), or direct numeric IP construction (inbound portal allowed).
#   6.  No mDNS, OTA, WiFiManager, or additional outbound client in
#       production source.
#   7.  No hardcoded external hostname, URL, or dotted-IP string literal other
#       than the exact two approved hosts and local portal IP 192.168.4.1 --
#       scanned in src/ + include/ AND in endpoint-defining platformio.ini build
#       flags (-D macros / URL / IP literals).
#   8.  README precisely discloses query precision, the six-decimal/0.1-NM
#       formatting being NOT a privacy-preserving reduction, per-party
#       observability (ADS-B provider vs DNS resolver vs SNTP service), local MAC
#       exposure, an explicit "does not send" credentials/telemetry negation,
#       DNS/DHCP, the temporary portal, and the source-gate versus
#       hardware-packet-capture limitation.
#   9.  DHCP NTP disable: esp_sntp_servermode_dhcp(false) present.
#  10.  No second configTime call in production source (counted with
#       \bconfigTime\s*\( so a spaced call cannot hide).
#  11.  Egress checks cover production C/C++/Arduino source and headers
#       (.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.ino).
#  12.  Executable call checks/counts operate on comments-and-strings-blanked
#       skeletons; strings-preserved text is used only where literal contents
#       (exact constants, the HTTP format string, host/URL/IP literals) are
#       intentionally inspected.
#
# Compatible with Windows PowerShell 5.1 and PowerShell 7.
# ===========================================================================

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

if (-not $ProjectRoot) {
  $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Fail {
  param([Parameter(Mandatory)][string]$Message)
  throw "EGRESS POLICY VIOLATION: $Message"
}

function Read-Source {
  param([Parameter(Mandatory)][string]$Path)
  if (-not (Test-Path $Path)) { Fail "Required file not found: $Path" }
  return Get-Content -Raw $Path
}

# C/C++ comment and string skeleton helper (same pattern as other gates).
# Returns text with block+line comments replaced by whitespace (newlines kept).
# With -BlankStrings, string/char literal interiors are also blanked.
function Get-CodeSkeleton {
  param(
    [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
    [switch]$BlankStrings
  )
  $pattern = "(?s)(`"(?:\\.|[^`"\\\r\n])*`")|('(?:\\.|[^'\\\r\n])*')|(//[^\r\n]*)|(/\*.*?\*/)"
  $rx = [regex]$pattern
  $blank = $BlankStrings.IsPresent
  $evaluator = {
    param($m)
    if ($m.Groups[1].Success -or $m.Groups[2].Success) {
      if (-not $blank) { return $m.Value }
      if ($m.Value.Length -le 2) { return $m.Value }
      $inner = $m.Value.Substring(1, $m.Value.Length - 2)
      $blanked = [regex]::Replace($inner, "[^\r\n]", " ")
      return $m.Value[0] + $blanked + $m.Value[$m.Value.Length - 1]
    }
    $replaced = [regex]::Replace($m.Value, "[^\r\n]", " ")
    return $replaced
  }
  return $rx.Replace($Text, $evaluator)
}

function Invoke-LiveGate {
  param([string]$Root)

  $configH   = Join-Path $Root "include\config.h"
  $readmeFile = Join-Path $Root "README.md"
  $srcDir    = Join-Path $Root "src"
  $incDir    = Join-Path $Root "include"

  $configText = Read-Source $configH
  $configCode = Get-CodeSkeleton $configText
  $readmeText = Read-Source $readmeFile
  $platformioFile = Join-Path $Root "platformio.ini"
  $platformioText = Read-Source $platformioFile

  # Scan every production C/C++/Arduino implementation/header extension.
  $sourceExtensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".ino")
  $prodFiles = @(
    Get-ChildItem -Recurse -File -Path $srcDir, $incDir |
      Where-Object { $sourceExtensions -contains $_.Extension.ToLowerInvariant() } |
      Select-Object -ExpandProperty FullName
  )

  # Build a combined skeleton (comments stripped, strings blanked) for
  # scanning that avoids false positives from PEM data and comments.
  $combinedSkel = ""
  foreach ($f in $prodFiles) {
    $txt = Get-Content -Raw $f
    $combinedSkel += (Get-CodeSkeleton $txt -BlankStrings) + "`n"
  }
  # -------------------------------------------------------------------------
  # 1. Exact approved host/port constants in config.h
  # -------------------------------------------------------------------------
  if ($configCode -notmatch '(?m)^\s*constexpr\s+char\s+kAdsbHost\s*\[\]\s*=\s*"opendata\.adsb\.fi"\s*;') {
    Fail 'config.h: kAdsbHost must be "opendata.adsb.fi"'
  }
  if ($configCode -notmatch '(?m)^\s*constexpr\s+uint16_t\s+kAdsbPort\s*=\s*443\s*;') {
    Fail "config.h: kAdsbPort must be 443"
  }
  if ($configCode -notmatch '(?m)^\s*constexpr\s+char\s+kSntpServerPrimary\s*\[\]\s*=\s*"time\.cloudflare\.com"\s*;') {
    Fail 'config.h: kSntpServerPrimary must be "time.cloudflare.com"'
  }

  # -------------------------------------------------------------------------
  # 2. Empty fallback SNTP constants
  # -------------------------------------------------------------------------
  if ($configCode -notmatch '(?m)^\s*constexpr\s+char\s+kSntpServerFallback1\s*\[\]\s*=\s*""\s*;') {
    Fail 'config.h: kSntpServerFallback1 must be "" (empty, policy-gated)'
  }
  if ($configCode -notmatch '(?m)^\s*constexpr\s+char\s+kSntpServerFallback2\s*\[\]\s*=\s*""\s*;') {
    Fail 'config.h: kSntpServerFallback2 must be "" (empty, policy-gated)'
  }

  # -------------------------------------------------------------------------
  # 3. DNS/TLS/HTTP Host derives from config::kAdsbHost
  # -------------------------------------------------------------------------
  # The connect call in adsb_client.cpp must use config::kAdsbHost.
  $adsbClientPath = Join-Path $srcDir "services\adsb_client.cpp"
  if (-not (Test-Path $adsbClientPath)) { Fail "adsb_client.cpp not found" }
  $adsbClientRaw  = Get-Content -Raw $adsbClientPath
  # Strings-and-comments-blanked skeleton: executable call checks cannot be
  # satisfied by string-literal contents.
  $adsbClientSkel = Get-CodeSkeleton $adsbClientRaw -BlankStrings
  # Comments-stripped, strings-preserved skeleton: used ONLY where the literal
  # HTTP format string content ("Host: %s") is intentionally inspected.
  $adsbClientCode = Get-CodeSkeleton $adsbClientRaw
  if ($adsbClientSkel -notmatch '(?s)espTlsConnect\s*\(\s*client\s*,\s*config::kAdsbHost\s*,\s*config::kAdsbPort\s*,') {
    Fail "adsb_client.cpp must pass config::kAdsbHost/config::kAdsbPort to espTlsConnect"
  }
  # Extract and BOUND the actual std::snprintf(http_request, ...) statement so the
  # Host %s argument check cannot span into the later espTlsConnect call's
  # config::kAdsbHost. The final format argument (the Host: %s substitution) must
  # be config::kAdsbHost; a hardcoded Host literal (blanked to spaces here since
  # strings are preserved but their contents cannot satisfy a symbol match) fails.
  $snprintfRx = [regex]'(?s)std::snprintf\s*\(\s*http_request\s*,.*?\)\s*;'
  $snMatch = $snprintfRx.Match($adsbClientCode)
  if (-not $snMatch.Success) {
    Fail "adsb_client.cpp: could not locate the std::snprintf(http_request, ...) statement"
  }
  $snStmt = $snMatch.Value
  if ($snStmt -notmatch '(?s)Host:\s*%s') {
    Fail "adsb_client.cpp: HTTP request must carry a 'Host: %s' header"
  }
  if ($snStmt -notmatch '(?s),\s*config::kAdsbHost\s*\)\s*;\s*$') {
    Fail "adsb_client.cpp: HTTP Host header argument must be config::kAdsbHost (final snprintf argument)"
  }
  $transportPath = Join-Path $srcDir "services\adsb_transport_esp.cpp"
  if (-not (Test-Path $transportPath)) { Fail "adsb_transport_esp.cpp not found" }
  $transportRaw  = Get-Content -Raw $transportPath
  $transportSkel = Get-CodeSkeleton $transportRaw -BlankStrings
  # Scope the resolver/connect chain to the actual espTlsConnect function body:
  # slice from its signature to the next known function definition so text in
  # another function, a string, or a comment cannot satisfy it.
  $espFnRx = [regex]'(?s)ConnectOutcome\s+espTlsConnect\s*\([^)]*\)\s*\{'
  $espFnMatch = $espFnRx.Match($transportSkel)
  if (-not $espFnMatch.Success) {
    Fail "adsb_transport_esp.cpp: could not locate espTlsConnect function body"
  }
  $espBodyStart = $espFnMatch.Index + $espFnMatch.Length
  $nextFnRx = [regex]'(?m)^\w[\w:<>&\*\s]*\b(?:espVerifyPeerCertValidity|espSendAll)\s*\('
  $nextFnMatch = $nextFnRx.Match($transportSkel, $espBodyStart)
  $espBodyEnd = if ($nextFnMatch.Success) { $nextFnMatch.Index } else { $transportSkel.Length }
  $espBody = $transportSkel.Substring($espBodyStart, $espBodyEnd - $espBodyStart)
  if ($espBody -notmatch '(?s)WiFi\.hostByName\s*\(\s*host\s*,\s*address\s*\)') {
    Fail "espTlsConnect must resolve the supplied host via WiFi.hostByName(host, address)"
  }
  if ($espBody -notmatch '(?s)client\.connect\s*\(\s*address\s*,\s*port\s*,\s*host\s*,\s*ca_bundle\s*,') {
    Fail "espTlsConnect must connect(address, port, host, ca_bundle, ...) with the resolved address"
  }

  # -------------------------------------------------------------------------
  # 4. configTime uses config SNTP constants; empty fallbacks become nullptr;
  #    esp_sntp_servermode_dhcp(false) is present.
  # -------------------------------------------------------------------------
  $timekeeperPath = Join-Path $srcDir "services\timekeeper.cpp"
  if (-not (Test-Path $timekeeperPath)) { Fail "timekeeper.cpp not found" }
  $timekeeperRaw  = Get-Content -Raw $timekeeperPath
  $timekeeperText = Get-CodeSkeleton $timekeeperRaw               # comments stripped, strings/chars kept
  $timekeeperSkel = Get-CodeSkeleton $timekeeperRaw -BlankStrings # strings+chars+comments blanked
  # Require executable code equivalent to the full SNTP server chain:
  #   s1 = config::kSntpServerPrimary
  #   s2 = kSntpServerFallback1[0] != '\0' ? kSntpServerFallback1 : nullptr
  #   s3 = kSntpServerFallback2[0] != '\0' ? kSntpServerFallback2 : nullptr
  # The char literal '\0' interiors are inspected, so the strings/chars-preserved
  # skeleton is used here; the configTime executable call is verified separately
  # on the strings-blanked skeleton so a string literal cannot fabricate it.
  $sntpChainRequired = @(
    @{ Pat = '=\s*config::kSntpServerPrimary\s*;'; Msg = "timekeeper.cpp must assign s1 = config::kSntpServerPrimary" },
    @{ Pat = "config::kSntpServerFallback1\s*\[\s*0\s*\]\s*!=\s*'\\0'\s*\?\s*config::kSntpServerFallback1\s*:\s*nullptr"; Msg = "timekeeper.cpp must derive s2 from kSntpServerFallback1 (empty => nullptr)" },
    @{ Pat = "config::kSntpServerFallback2\s*\[\s*0\s*\]\s*!=\s*'\\0'\s*\?\s*config::kSntpServerFallback2\s*:\s*nullptr"; Msg = "timekeeper.cpp must derive s3 from kSntpServerFallback2 (empty => nullptr)" }
  )
  foreach ($req in $sntpChainRequired) {
    if ($timekeeperText -notmatch $req.Pat) { Fail $req.Msg }
  }
  # The configTime call must pass (0, 0, s1, s2, s3) as executable code.
  if ($timekeeperSkel -notmatch 'configTime\s*\(\s*0\s*,\s*0\s*,\s*\w+\s*,\s*\w+\s*,\s*\w+\s*\)') {
    Fail "timekeeper.cpp must call configTime(0, 0, s1, s2, s3)"
  }
  # Ban alternate SNTP server configuration paths. The existing time-sync
  # notification callback (sntp_set_time_sync_notification_cb) remains allowed.
  foreach ($banned in @('\bconfigTzTime\s*\(', '\bsntp_setservername\s*\(', '\besp_sntp_setservername\s*\(')) {
    if ($timekeeperSkel -match $banned) {
      Fail "timekeeper.cpp contains banned alternate SNTP configuration API: $banned"
    }
  }

  # -------------------------------------------------------------------------
  # 9. DHCP NTP disable (checked here as part of timekeeper verification)
  # -------------------------------------------------------------------------
  if ($timekeeperSkel -notmatch [regex]::Escape("esp_sntp_servermode_dhcp(false)")) {
    Fail "timekeeper.cpp: esp_sntp_servermode_dhcp(false) must be present (not just in a comment)"
  }

  # -------------------------------------------------------------------------
  # 10. No second configTime call in production source. Count on the
  #     strings-blanked skeleton with \bconfigTime\s*\( so whitespace cannot hide
  #     a spaced second call and a string literal cannot fabricate one.
  # -------------------------------------------------------------------------
  $configTimeCalls = [regex]::Matches($combinedSkel, '\bconfigTime\s*\(')
  if ($configTimeCalls.Count -gt 1) {
    Fail "Production source has more than one configTime call ($($configTimeCalls.Count))"
  }
  if ($configTimeCalls.Count -eq 0) {
    Fail "Production source has no configTime call"
  }

  # -------------------------------------------------------------------------
  # 5. No HTTPClient, WiFiUDP, raw socket, or direct external IP connect
  #    in production source (outside captive portal allowed patterns).
  # -------------------------------------------------------------------------
  # HTTPClient: banned entirely from production (captive portal uses raw lwip)
  if ($combinedSkel -match '\bHTTPClient\b') {
    Fail "Production source contains HTTPClient (banned)"
  }
  # WiFiUDP: banned (SNTP goes through esp-idf configTime, not raw UDP socket)
  if ($combinedSkel -match '\bWiFiUDP\b') {
    Fail "Production source contains WiFiUDP (banned)"
  }
  $dnsCalls = [regex]::Matches($combinedSkel, '\bWiFi\.hostByName\s*\(')
  if ($dnsCalls.Count -ne 1) {
    Fail "Production source must contain exactly one WiFi.hostByName call; found $($dnsCalls.Count)"
  }
  $connectCalls = [regex]::Matches($combinedSkel, '\.\s*connect\s*\(')
  if ($connectCalls.Count -ne 1) {
    Fail "Production source must contain exactly one outbound client .connect call; found $($connectCalls.Count)"
  }
  foreach ($pat in @(
    '(?<![\w.])socket\s*\(', '(?<![\w.])lwip_socket\s*\(',
    '(?<![\w.])connect\s*\(', '(?<![\w.])lwip_connect\s*\(',
    '(?<![\w.])sendto\s*\(', '(?<![\w.])lwip_sendto\s*\(',
    '\bAsyncClient\b', '\bAsyncTCP\b', '\bEthernetClient\b',
    '\bPubSubClient\b', '\bMQTTClient\b', '\bWebSocketsClient\b',
    # Additional high-signal outbound/DNS APIs (none used by production code; the
    # local captive-portal lwip_recv/lwip_send path stays allowed).
    '\besp_http_client\w*\b', '\besp_tls_conn\w*\s*\(',
    '\bgetaddrinfo\s*\(', '\bdns_gethostbyname\s*\(',
    '\btcp_connect\s*\(', '\budp_connect\s*\(',
    '\bAsyncUDP\b', '\bNetworkClient\b', '\bNetworkUDP\b'
  )) {
    if ($combinedSkel -match $pat) {
      Fail "Production source contains banned outbound socket/client API: $pat"
    }
  }
  $numericIpRx = [regex]'IPAddress\s*\(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*\)'
  foreach ($m in $numericIpRx.Matches($combinedSkel)) {
    if ($m.Value -notmatch '^IPAddress\s*\(\s*192\s*,\s*168\s*,\s*4\s*,\s*1\s*\)$') {
      Fail "Production source contains direct numeric IP construction: $($m.Value)"
    }
  }

  # -------------------------------------------------------------------------
  # 6. No mDNS, OTA, WiFiManager in production source
  # -------------------------------------------------------------------------
  # Check comment-stripped skeleton to avoid false positives in comments
  foreach ($pat in @("ESPmDNS", "ArduinoOTA", "ElegantOTA",
                     "WiFiManager", "WebServer\s*webServer")) {
    if ($combinedSkel -imatch $pat) {
      Fail "Production source contains disallowed library: $pat"
    }
  }
  # include directives for these libraries (in skeleton after blanking)
  foreach ($pat in @('#include.*mDNS', '#include.*OTA', '#include.*WiFiManager')) {
    if ($combinedSkel -imatch $pat) {
      Fail "Production source includes disallowed header: $pat"
    }
  }

  # -------------------------------------------------------------------------
  # 7. No hardcoded external hostname/IP other than approved parties
  # -------------------------------------------------------------------------
  # Approved: "opendata.adsb.fi", "time.cloudflare.com", "192.168.4.1" variants
  # Scan strings in comment-stripped (strings kept) source.
  # We blank the combined skeleton to find all string literals.
  # Strategy: scan for string literals; after blanking adsb_ca_bundle.cpp (PEM),
  # any remaining dotted-IP or external host string is suspicious.
  $approvedHosts = @("opendata.adsb.fi", "time.cloudflare.com")
  # Pattern: quoted string that looks like an external hostname (TLD >= 2 alpha chars)
  # Requires at least one dot, TLD consisting of 2+ alpha chars only.
  # This excludes .h, .cpp, format strings, NVS keys, etc.
  $suspiciousHostRx = [regex]'"([a-zA-Z][a-zA-Z0-9][-a-zA-Z0-9]*(?:\.[a-zA-Z0-9][-a-zA-Z0-9]*)*\.[a-zA-Z]{2,})"'
  $suspiciousIpRx   = [regex]'"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"'

  foreach ($f in $prodFiles) {
    $fname = [System.IO.Path]::GetFileName($f)
    # Skip PEM/CA bundle files (certificate data contains many hostnames)
    if ($fname -match "ca_bundle" -or $fname -match "adsb_ca") { continue }
    $fText = Get-Content -Raw $f
    $fSkel = Get-CodeSkeleton $fText  # comments stripped, strings kept
    $hostMatches = $suspiciousHostRx.Matches($fSkel)
    foreach ($m in $hostMatches) {
      $hostname = $m.Groups[1].Value
      $isApproved = $false
      foreach ($a in $approvedHosts) {
        if ($hostname -eq $a) { $isApproved = $true; break }
      }
      if (-not $isApproved) {
        Fail "Production file '$fname' contains potentially external hostname: '$hostname'"
      }
    }
    # Check for dotted IPs that aren't the portal address
    $ipMatches = $suspiciousIpRx.Matches($fSkel)
    foreach ($m in $ipMatches) {
      $ip = $m.Groups[1].Value
      $isApproved = $false
      $isApproved = $ip -eq "192.168.4.1"
      if (-not $isApproved) {
        Fail "Production file '$fname' contains hardcoded external IP: '$ip'"
      }
    }
    $urlRx = [regex]'"(?:https?|wss?)://([^/"\s]+)'
    foreach ($m in $urlRx.Matches($fSkel)) {
      $urlHost = $m.Groups[1].Value
      if (($approvedHosts -notcontains $urlHost) -and $urlHost -ne "192.168.4.1") {
        Fail "Production file '$fname' contains external URL literal: '$($m.Value)'"
      }
    }
  }

  # platformio.ini is part of the declared production scope: an endpoint-defining
  # build flag/macro (e.g. -DSOME_HOST="evil.example" or a URL/IP baked into a
  # -D define) would silently introduce a runtime destination the C/C++ scan
  # never sees. Scan build-flag define (-D...) lines for host/URL/IP endpoints.
  foreach ($rawLine in ($platformioText -split "`r?`n")) {
    $line = $rawLine
    if ($line -notmatch '-D') { continue }  # focus on build-flag define lines
    # -Dname="host.tld" / -Dname='host.tld' macro endpoint literals
    foreach ($dm in ([regex]'-D\s*\w+\s*=\s*(["''])([^"'']*)\1').Matches($line)) {
      $val = $dm.Groups[2].Value
      if ($val -match '^(?:https?|wss?)://([^/\s]+)') {
        $h = $Matches[1]
        if (($approvedHosts -notcontains $h) -and $h -ne "192.168.4.1") {
          Fail "platformio.ini build flag defines external URL endpoint: '$val'"
        }
      }
      elseif ($val -match '^[a-zA-Z][a-zA-Z0-9-]*(?:\.[a-zA-Z0-9-]+)*\.[a-zA-Z]{2,}$' -and
              ($approvedHosts -notcontains $val)) {
        Fail "platformio.ini build flag defines external hostname endpoint: '$val'"
      }
      elseif ($val -match '^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$' -and $val -ne "192.168.4.1") {
        Fail "platformio.ini build flag defines external IP endpoint: '$val'"
      }
    }
    # bare URL / dotted-IP endpoint literals on a build-flag line
    foreach ($um in ([regex]'(?:https?|wss?)://([^/\s"'']+)').Matches($line)) {
      $h = $um.Groups[1].Value
      if (($approvedHosts -notcontains $h) -and $h -ne "192.168.4.1") {
        Fail "platformio.ini contains external URL endpoint literal: '$($um.Value)'"
      }
    }
    foreach ($ipm in ([regex]'\b(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})\b').Matches($line)) {
      if ($ipm.Groups[1].Value -ne "192.168.4.1") {
        Fail "platformio.ini contains external IP endpoint literal: '$($ipm.Groups[1].Value)'"
      }
    }
  }

  # -------------------------------------------------------------------------
  # 8. README has runtime egress table and privacy disclosure
  # -------------------------------------------------------------------------
  if ($readmeText -notmatch "opendata\.adsb\.fi" -or
      $readmeText -notmatch "TCP.*443") {
    Fail "README missing ADS-B runtime egress entry (opendata.adsb.fi / TCP/443)"
  }
  if ($readmeText -notmatch "time\.cloudflare\.com" -or
      $readmeText -notmatch "UDP.*123|123.*UDP") {
    Fail "README missing SNTP runtime egress entry (time.cloudflare.com / UDP/123)"
  }
  # Explicit negation: the "does not send" block must actually negate sending
  # credentials/telemetry to an external service. Affirmative wording that merely
  # mentions the keywords must NOT satisfy this.
  $negBlockRx = [regex]'(?is)does not send:?\s*(?<body>.*?)(?:\r?\n\r?\n|\r?\n#{1,6}\s)'
  $negMatch = $negBlockRx.Match($readmeText)
  if (-not $negMatch.Success) {
    Fail "README must contain an explicit 'does not send' negation block"
  }
  $negBody = $negMatch.Groups['body'].Value
  foreach ($kw in @('credential', 'telemetry')) {
    if ($negBody -notmatch "(?is)$kw") {
      Fail "README 'does not send' negation block must explicitly cover: $kw"
    }
  }
  if ($negBody -notmatch '(?is)external service') {
    Fail "README 'does not send' negation block must reference 'external service'"
  }
  $readmeRequired = @(
    '(?is)ADS-B query parameters.*lat/lon.*formatted/rounded.*six decimal places.*radius.*0\.1 NM',
    '(?is)MAC address.*local network',
    '(?is)DNS/DHCP infrastructure',
    '(?is)temporary.*session-scoped SoftAP',
    '(?is)source-only.*packet capture.*final runtime proof',
    # Six-decimal / 0.1-NM formatting must be disclosed as NOT a privacy-preserving
    # precision reduction.
    '(?is)six decimal.*not.*privacy-preserving precision reduction',
    # The three external parties observe DIFFERENT things; DNS/SNTP must NOT be
    # described as seeing the HTTPS query path.
    '(?is)ADS-B provider.*source address.*lat/lon',
    '(?is)DNS resolver.*source address.*hostname',
    '(?is)SNTP service.*source address.*NTP'
  )
  foreach ($required in $readmeRequired) {
    if ($readmeText -notmatch $required) {
      Fail "README missing required precise egress/privacy disclosure: $required"
    }
  }

  Write-Host "OK: all 12 egress policy invariants verified."
  Write-Host ""
  Write-Host "NOTE: This gate is OFFLINE/source-only."
  Write-Host "      Hardware packet capture is the final runtime proof."
  Write-Host "      Build-time OurAirports/GitHub URLs in scripts/ are not"
  Write-Host "      runtime firmware egress and are not checked here."
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
function Invoke-SelfTest {
  param([string]$Root)

  $configH    = Join-Path $Root "include\config.h"
  $readmeFile = Join-Path $Root "README.md"
  $timekeeperPath = Join-Path $Root "src\services\timekeeper.cpp"
  $adsbClientPath = Join-Path $Root "src\services\adsb_client.cpp"

  $configOrig     = Get-Content -Raw $configH
  $readmeOrig     = Get-Content -Raw $readmeFile
  $timekeeperOrig = Get-Content -Raw $timekeeperPath
  $adsbClientOrig = Get-Content -Raw $adsbClientPath
  $transportPath   = Join-Path $Root "src\services\adsb_transport_esp.cpp"
  $transportOrig   = Get-Content -Raw $transportPath
  $platformioPath  = Join-Path $Root "platformio.ini"
  $platformioOrig  = Get-Content -Raw $platformioPath

  $tmp = [System.IO.Path]::Combine(
    $env:TEMP,
    "egress-gate-selftest-$([System.Guid]::NewGuid().ToString('N'))"
  )
  # Mirror the directory structure needed by the gate
  $dirs = @(
    (Join-Path $tmp "include\services"),
    (Join-Path $tmp "include\core"),
    (Join-Path $tmp "include\ui"),
    (Join-Path $tmp "include\hardware"),
    (Join-Path $tmp "include\data"),
    (Join-Path $tmp "src\services"),
    (Join-Path $tmp "src\core"),
    (Join-Path $tmp "src\ui"),
    (Join-Path $tmp "src\hardware"),
    (Join-Path $tmp "src\data")
  )
  foreach ($d in $dirs) { New-Item -ItemType Directory -Path $d -Force | Out-Null }

  # Copy all production source files to the temp tree, using the SAME extension
  # set the live gate scans (.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.ino), so self-test
  # coverage matches the live scope.
  $selfTestExtensions = @("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.ino")
  foreach ($f in (Get-ChildItem -Recurse -Path (Join-Path $Root "src") -Include $selfTestExtensions)) {
    $rel = $f.FullName.Substring((Join-Path $Root "src").Length + 1)
    $dest = Join-Path (Join-Path $tmp "src") $rel
    $destDir = Split-Path $dest -Parent
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    Copy-Item $f.FullName $dest
  }
  foreach ($f in (Get-ChildItem -Recurse -Path (Join-Path $Root "include") -Include $selfTestExtensions)) {
    $rel = $f.FullName.Substring((Join-Path $Root "include").Length + 1)
    $dest = Join-Path (Join-Path $tmp "include") $rel
    $destDir = Split-Path $dest -Parent
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    Copy-Item $f.FullName $dest
  }
  Copy-Item $readmeFile (Join-Path $tmp "README.md")
  Copy-Item $platformioPath (Join-Path $tmp "platformio.ini")

  function Write-TmpConfig([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "include\config.h"), $content)
  }
  function Write-TmpPlatformio([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "platformio.ini"), $content)
  }
  function Write-TmpTimekeeper([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "src\services\timekeeper.cpp"), $content)
  }
  function Write-TmpAdsbClient([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "src\services\adsb_client.cpp"), $content)
  }
  function Write-TmpTransport([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "src\services\adsb_transport_esp.cpp"), $content)
  }
  function Write-TmpReadme([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "README.md"), $content)
  }
  function Reset-Tmp {
    Write-TmpConfig $configOrig
    Write-TmpTimekeeper $timekeeperOrig
    Write-TmpAdsbClient $adsbClientOrig
    Write-TmpTransport $transportOrig
    Write-TmpReadme $readmeOrig
    Write-TmpPlatformio $platformioOrig
    # Remove any stray tamper files dropped by extension-coverage tests.
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $tmp "src\services\extra_egress.ino")
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $tmp "src\services\extra_egress.cc")
  }

  function Expect-Fail([string]$Label) {
    try {
      Invoke-LiveGate -Root $tmp
      throw "SELF-TEST FAILURE: '$Label' should have failed but passed."
    } catch {
      if ($_.Exception.Message -match "SELF-TEST FAILURE") { throw }
      Write-Host "OK (rejected): $Label"
    }
  }

  try {
    Reset-Tmp

    # T1: non-empty SNTP fallback
    Write-TmpConfig ($configOrig -replace [regex]::Escape('kSntpServerFallback1[] = ""'),
      'kSntpServerFallback1[] = "ntp.example.com"')
    Expect-Fail "non-empty SNTP fallback1"
    Reset-Tmp

    # T2: changed ADS-B host
    Write-TmpConfig ($configOrig -replace [regex]::Escape('"opendata.adsb.fi"'), '"data.adsb.one"')
    Expect-Fail "changed ADS-B host"
    Reset-Tmp

    # T3: changed SNTP primary host
    Write-TmpConfig ($configOrig -replace [regex]::Escape('"time.cloudflare.com"'), '"pool.ntp.org"')
    Expect-Fail "changed SNTP host"
    Reset-Tmp

    # T4: hardcoded external literal in adsb_client
    $badClient4 = $adsbClientOrig + "`nstatic const char* kExtraHost = `"data.extra.com`";`n"
    Write-TmpAdsbClient $badClient4
    Expect-Fail "extra external hostname literal"
    Reset-Tmp

    # T5: direct external IP in adsb_client
    $badClient5 = $adsbClientOrig + "`nconst char* kDirectIP = `"1.2.3.4`";`n"
    Write-TmpAdsbClient $badClient5
    Expect-Fail "direct external IP literal"
    Reset-Tmp

    # T6: second configTime call
    $badTk6 = $timekeeperOrig + "`nvoid rearmSntp() { configTime(0,0,`"extra.ntp.org`"); }`n"
    Write-TmpTimekeeper $badTk6
    Expect-Fail "second configTime call"
    Reset-Tmp

    # T7: removed DHCP-NTP disable
    $badTk7 = $timekeeperOrig -replace [regex]::Escape("esp_sntp_servermode_dhcp(false)"), "/* esp_sntp_servermode_dhcp(false) */"
    Write-TmpTimekeeper $badTk7
    Expect-Fail "removed DHCP-NTP disable"
    Reset-Tmp

    # T8: HTTPClient in production source
    $badClient8 = $adsbClientOrig + "`nHTTPClient httpClient;`n"
    Write-TmpAdsbClient $badClient8
    Expect-Fail "HTTPClient in production source"
    Reset-Tmp

    # T9: mDNS include in production source
    $badClient9 = $adsbClientOrig + "`n#include <ESPmDNS.h>`n"
    Write-TmpAdsbClient $badClient9
    Expect-Fail "mDNS include in production source"
    Reset-Tmp

    # T10: removed privacy docs from README
    $readmeLines = $readmeOrig -split "`n"
    $filteredLines = $readmeLines | Where-Object { $_ -notmatch "telemetry" }
    $badReadme10 = $filteredLines -join "`n"
    Write-TmpReadme $badReadme10
    Expect-Fail "removed telemetry disclosure from README"
    Reset-Tmp

    # T11: numeric construction must not provide a direct external endpoint.
    $badClient11 = $adsbClientOrig + "`nconst IPAddress direct_ip = IPAddress(1, 2, 3, 4);`n"
    Write-TmpAdsbClient $badClient11
    Expect-Fail "numeric external IP construction"
    Reset-Tmp

    # T12: URLs must be checked by host, not missed as non-hostname strings.
    $badClient12 = $adsbClientOrig + "`nconst char* kExtraUrl = `"https://evil.example/path`";`n"
    Write-TmpAdsbClient $badClient12
    Expect-Fail "external URL literal"
    Reset-Tmp

    # T13: an approved hostname prefix must not be treated as an exact host.
    $badClient13 = $adsbClientOrig + "`nconst char* kPrefixHost = `"opendata.adsb.fi.evil.com`";`n"
    Write-TmpAdsbClient $badClient13
    Expect-Fail "hostname prefix trick"
    Reset-Tmp

    # T14: the actual resolver/connect chain cannot be replaced by a comment.
    Write-TmpTransport ($transportOrig -replace [regex]::Escape("WiFi.hostByName(host, address)"),
      "/* WiFi.hostByName(host, address) */ false")
    Expect-Fail "broken DNS-connect chain"
    Reset-Tmp

    # T15: generic telemetry wording cannot substitute for precise query disclosure.
    Write-TmpReadme ($readmeOrig -replace '(?m)^\| ADS-B query parameters \|.*\r?\n', '')
    Expect-Fail "removed precise ADS-B query disclosure"
    Reset-Tmp

    # T16: generic telemetry wording cannot substitute for the capture caveat.
    Write-TmpReadme ($readmeOrig -replace '(?m)^.*packet capture.*\r?\n', '')
    Expect-Fail "removed packet-capture caveat"
    Reset-Tmp

    # T17: a removed executable WiFi.hostByName call plus a matching STRING literal
    # must NOT satisfy the gate (executable checks run on strings-blanked skeleton).
    Write-TmpTransport ($transportOrig -replace [regex]::Escape("if (!WiFi.hostByName(host, address)) {"),
      'const char* kFake = "WiFi.hostByName(host, address)"; if (false) {')
    Expect-Fail "string literal cannot fabricate WiFi.hostByName call"
    Reset-Tmp

    # T18: a removed client.connect executable call plus a matching STRING literal
    # must NOT satisfy the gate.
    Write-TmpTransport ($transportOrig -replace [regex]::Escape("const int ok = client.connect(address, port, host, ca_bundle, nullptr, nullptr);"),
      'const char* kFake = "client.connect(address, port, host, ca_bundle, ...)"; const int ok = 0;')
    Expect-Fail "string literal cannot fabricate client.connect call"
    Reset-Tmp

    # T19: tampering ONLY the HTTP Host-header argument (leaving the TLS connect
    # to config::kAdsbHost intact) must fail: the snprintf's final format argument
    # is no longer config::kAdsbHost.
    Write-TmpAdsbClient ($adsbClientOrig -replace [regex]::Escape("config::kAdsbHost);"), '"opendata.adsb.fi");')
    Expect-Fail "Host-header argument decoupled from config::kAdsbHost"
    Reset-Tmp

    # T20: a WiFi.hostByName call in ANOTHER function (espSendAll) must not satisfy
    # the espTlsConnect-scoped chain check even though the global count stays one.
    $badTransport20 = ($transportOrig -replace [regex]::Escape("if (!WiFi.hostByName(host, address)) {"), "if (false) {") `
      -replace [regex]::Escape("  size_t sent = 0;"), "  WiFi.hostByName(host, address);`n  size_t sent = 0;"
    Write-TmpTransport $badTransport20
    Expect-Fail "hostByName outside espTlsConnect body"
    Reset-Tmp

    # T21: breaking the fallback2 ternary (no empty => nullptr guard) must fail.
    Write-TmpTimekeeper ($timekeeperOrig -replace [regex]::Escape("config::kSntpServerFallback2[0] != '\0' ? config::kSntpServerFallback2 : nullptr"),
      "config::kSntpServerFallback2")
    Expect-Fail "broken SNTP fallback2 ternary"
    Reset-Tmp

    # T22: a spaced second configTime ( call must be counted and rejected.
    Write-TmpTimekeeper ($timekeeperOrig + "`nvoid rearm2() { configTime (0, 0, `"x`"); }`n")
    Expect-Fail "spaced second configTime call"
    Reset-Tmp

    # T23: an alternate SNTP configuration API (configTzTime) is banned.
    Write-TmpTimekeeper ($timekeeperOrig + "`nvoid altSntp() { configTzTime(`"UTC0`", config::kSntpServerPrimary); }`n")
    Expect-Fail "banned configTzTime SNTP path"
    Reset-Tmp

    # T24: an alternate outbound/DNS API (getaddrinfo) is banned.
    Write-TmpAdsbClient ($adsbClientOrig + "`nvoid alt() { getaddrinfo(`"h`", `"443`", nullptr, nullptr); }`n")
    Expect-Fail "banned getaddrinfo outbound API"
    Reset-Tmp

    # T25: an endpoint-defining build flag in platformio.ini is caught.
    Write-TmpPlatformio ($platformioOrig -replace [regex]::Escape("-DARDUINO_USB_MODE=1"), '-DADSB_HOST="evil.example.com"')
    Expect-Fail "platformio.ini build-flag endpoint"
    Reset-Tmp

    # T26: a banned external URL in a NON-.cpp/.h extension (.ino) is scanned,
    # proving the live extension coverage is real.
    [System.IO.File]::WriteAllText((Join-Path $tmp "src\services\extra_egress.ino"),
      'namespace { const char* kEvil = "https://evil.example/path"; }' + "`n")
    Expect-Fail "banned URL in .ino extension"
    Reset-Tmp

    # T27: flipping the "does not send" negation (while retaining every keyword)
    # must fail: affirmative wording cannot pass the privacy negation gate.
    Write-TmpReadme ($readmeOrig -replace [regex]::Escape("The firmware does not send:"), "The firmware does send:")
    Expect-Fail "flipped does-not-send negation"
    Reset-Tmp

    Write-Host ""
    Write-Host "OK: all 27 self-test tamper cases correctly rejected."
  } finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  }
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
if ($SelfTest) {
  Invoke-SelfTest -Root $ProjectRoot
} else {
  Invoke-LiveGate -Root $ProjectRoot
}
