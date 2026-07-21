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
#       call site through espTlsConnect, WiFi.hostByName(host, address), and
#       client.connect(address, port, host, ca_bundle, ...) to HTTP Host.
#   4.  configTime path uses config SNTP constants; empty fallbacks become
#       nullptr; esp_sntp_servermode_dhcp(false) is present.
#   5.  Exactly one production DNS and outbound client-connect path; no
#       HTTPClient, WiFiUDP, raw outbound socket, alternate client, or direct
#       numeric IP construction (the inbound portal remains allowed).
#   6.  No mDNS, OTA, WiFiManager, or additional outbound client in
#       production source.
#   7.  No hardcoded external hostname, URL, or dotted-IP string literal other
#       than the exact two approved hosts and local portal IP 192.168.4.1.
#   8.  README precisely discloses query precision, local MAC exposure, no
#       credentials/telemetry externally, DNS/DHCP, temporary portal, and the
#       source-gate versus hardware-packet-capture limitation.
#   9.  DHCP NTP disable: esp_sntp_servermode_dhcp(false) present.
#  10.  No second configTime call in production source.
#  11.  Egress checks cover production C/C++/Arduino source and headers
#       (.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.ino).
#  12.  All constant/chain checks operate on comment-stripped code.
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
  # Also a version with strings preserved (for Host/connect argument checks)
  $combinedRaw = ""
  foreach ($f in $prodFiles) {
    $combinedRaw += (Get-Content -Raw $f) + "`n"
  }
  # Comments stripped, strings kept (for string content checks)
  $combinedCommentStripped = ""
  foreach ($f in $prodFiles) {
    $txt = Get-Content -Raw $f
    $combinedCommentStripped += (Get-CodeSkeleton $txt) + "`n"
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
  # The connect call in adsb_client.cpp must use config::kAdsbHost
  $adsbClientPath = Join-Path $srcDir "services\adsb_client.cpp"
  if (-not (Test-Path $adsbClientPath)) { Fail "adsb_client.cpp not found" }
  $adsbClientCode = Get-CodeSkeleton (Get-Content -Raw $adsbClientPath)
  if ($adsbClientCode -notmatch '(?s)espTlsConnect\s*\(\s*client\s*,\s*config::kAdsbHost\s*,\s*config::kAdsbPort\s*,') {
    Fail "adsb_client.cpp must pass config::kAdsbHost/config::kAdsbPort to espTlsConnect"
  }
  # The snprintf Host argument must be config::kAdsbHost, not a hardcoded string.
  $hostPatRx = [regex]"(?s)Host:\s*%s.*?config::kAdsbHost"
  if (-not $hostPatRx.IsMatch($adsbClientCode)) {
    Fail "adsb_client.cpp: HTTP Host header must be derived from config::kAdsbHost"
  }
  $transportPath = Join-Path $srcDir "services\adsb_transport_esp.cpp"
  if (-not (Test-Path $transportPath)) { Fail "adsb_transport_esp.cpp not found" }
  $transportCode = Get-CodeSkeleton (Get-Content -Raw $transportPath)
  if ($transportCode -notmatch '(?s)WiFi\.hostByName\s*\(\s*host\s*,\s*address\s*\)' -or
      $transportCode -notmatch '(?s)client\.connect\s*\(\s*address\s*,\s*port\s*,\s*host\s*,\s*ca_bundle\s*,') {
    Fail "espTlsConnect must resolve supplied host and connect(address, port, host, ca_bundle, ...)"
  }

  # -------------------------------------------------------------------------
  # 4. configTime uses config SNTP constants; empty fallbacks become nullptr;
  #    esp_sntp_servermode_dhcp(false) is present.
  # -------------------------------------------------------------------------
  $timekeeperPath = Join-Path $srcDir "services\timekeeper.cpp"
  if (-not (Test-Path $timekeeperPath)) { Fail "timekeeper.cpp not found" }
  $timekeeperText = Get-CodeSkeleton (Get-Content -Raw $timekeeperPath)
  if ($timekeeperText -notmatch [regex]::Escape("config::kSntpServerPrimary")) {
    Fail "timekeeper.cpp must use config::kSntpServerPrimary"
  }
  if ($timekeeperText -notmatch [regex]::Escape("config::kSntpServerFallback1")) {
    Fail "timekeeper.cpp must reference config::kSntpServerFallback1"
  }
  if ($timekeeperText -notmatch "nullptr") {
    Fail "timekeeper.cpp: empty fallbacks must become nullptr"
  }

  # -------------------------------------------------------------------------
  # 9. DHCP NTP disable (checked here as part of timekeeper verification)
  # -------------------------------------------------------------------------
  $timekeeperSkel = Get-CodeSkeleton $timekeeperText
  if ($timekeeperSkel -notmatch [regex]::Escape("esp_sntp_servermode_dhcp(false)")) {
    Fail "timekeeper.cpp: esp_sntp_servermode_dhcp(false) must be present (not just in a comment)"
  }

  # -------------------------------------------------------------------------
  # 10. No second configTime call in production source
  # -------------------------------------------------------------------------
  $configTimeCalls = [regex]::Matches($combinedCommentStripped, [regex]::Escape("configTime("))
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
  $dnsCalls = [regex]::Matches($combinedCommentStripped, '\bWiFi\.hostByName\s*\(')
  if ($dnsCalls.Count -ne 1) {
    Fail "Production source must contain exactly one WiFi.hostByName call; found $($dnsCalls.Count)"
  }
  $connectCalls = [regex]::Matches($combinedCommentStripped, '\.\s*connect\s*\(')
  if ($connectCalls.Count -ne 1) {
    Fail "Production source must contain exactly one outbound client .connect call; found $($connectCalls.Count)"
  }
  foreach ($pat in @(
    '(?<![\w.])socket\s*\(', '(?<![\w.])lwip_socket\s*\(',
    '(?<![\w.])connect\s*\(', '(?<![\w.])lwip_connect\s*\(',
    '(?<![\w.])sendto\s*\(', '(?<![\w.])lwip_sendto\s*\(',
    '\bAsyncClient\b', '\bAsyncTCP\b', '\bEthernetClient\b',
    '\bPubSubClient\b', '\bMQTTClient\b', '\bWebSocketsClient\b'
  )) {
    if ($combinedSkel -match $pat) {
      Fail "Production source contains banned outbound socket/client API: $pat"
    }
  }
  $numericIpRx = [regex]'IPAddress\s*\(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+\s*\)'
  foreach ($m in $numericIpRx.Matches($combinedCommentStripped)) {
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
    if ($combinedCommentStripped -imatch $pat) {
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
  $readmeRequired = @(
    '(?is)ADS-B query parameters.*lat/lon.*formatted/rounded.*six decimal places.*radius.*0\.1 NM',
    '(?is)MAC address.*local network',
    '(?is)credentials.*external service',
    '(?is)telemetry.*external service',
    '(?is)DNS/DHCP infrastructure',
    '(?is)temporary.*session-scoped SoftAP',
    '(?is)source-only.*packet capture.*final runtime proof'
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

  # Copy all production source files to the temp tree
  foreach ($f in (Get-ChildItem -Recurse -Path (Join-Path $Root "src") -Include "*.cpp","*.h")) {
    $rel = $f.FullName.Substring((Join-Path $Root "src").Length + 1)
    $dest = Join-Path (Join-Path $tmp "src") $rel
    $destDir = Split-Path $dest -Parent
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    Copy-Item $f.FullName $dest
  }
  foreach ($f in (Get-ChildItem -Recurse -Path (Join-Path $Root "include") -Include "*.h")) {
    $rel = $f.FullName.Substring((Join-Path $Root "include").Length + 1)
    $dest = Join-Path (Join-Path $tmp "include") $rel
    $destDir = Split-Path $dest -Parent
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    Copy-Item $f.FullName $dest
  }
  Copy-Item $readmeFile (Join-Path $tmp "README.md")

  function Write-TmpConfig([string]$content) {
    [System.IO.File]::WriteAllText((Join-Path $tmp "include\config.h"), $content)
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

    Write-Host ""
    Write-Host "OK: all 16 self-test tamper cases correctly rejected."
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
