[CmdletBinding()]
param(
  # Root of the tree to verify. Defaults to the repository root. The negative
  # tamper self-test points this at an isolated temporary copy of src/ + include/
  # + platformio.ini so it can prove the gate REJECTS regressions without ever
  # mutating the worktree.
  [string]$ProjectRoot,
  # Run the in-repo negative tamper self-test (proves the gate fails on
  # representative regressions) instead of the live gate.
  [switch]$SelfTest
)

# ===========================================================================
# Phase 8 provisioning policy / tamper gate.
#
# Deterministic, OFFLINE verification that the Phase 8 secure-provisioning
# release invariants hold in PRODUCTION SOURCE (src/ + include/ + platformio.ini)
# -- never trusting comments or README. It needs no compiler, no network and no
# device: it parses source only. Every violation throws (non-zero exit).
#
# It proves, from source alone:
#   1. No WiFiManager/OTA/mDNS/broad-WebServer surface (dependency or code).
#   2. The captive HTTP route set is exactly the 10 allowed method/path pairs,
#      with the WiFiManager/OTA deny-list intact and no wildcard/prefix route.
#   3. Exactly one production listener (the AP-bound WiFiServer + DNSServer in
#      config_portal.cpp), one services::portal::start caller (wifi_setup.cpp),
#      main.cpp starts no server, and stop() tears down server + DNS.
#   4. No provisioning file logs a password/PSK/CSRF/token/candidate field, and
#      the portal + credential modules secureZero their owned workspaces.
#   5. RAM/FLASH storage transaction policy: esp_wifi_set_storage is confined to
#      wifi_credentials.cpp, runtime declares WiFi.persistent(false), FLASH is
#      selected only by the commit path + verified erase helper, candidate/restore
#      require RAM, eraseSta restores RAM, and durable markers precede + gate the
#      flash mutation.
#   6. No insecure AP teardown: no softAPdisconnect / WiFi.softAP( / eraseap=true,
#      and secure AP creation is raw stop -> AP mode -> secured config -> start.
#   7. Exactly one full-frame LGFX_Sprite and one createSprite (one framebuffer).
#   8. The config portal stays bounded: fixed HTTP limits, no String/malloc/new,
#      exact-IP one-client server, lwip_recv/lwip_send, start-failure end(), and a
#      stop() that closes server + DNS.
#
# Compatible with Windows PowerShell 5.1 and PowerShell 7. All parsing is
# in-memory; the self-test's only temporary files live in an isolated copy that
# is always removed in a finally block.
# ===========================================================================

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

if (-not $ProjectRoot) {
  $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Fail {
  param([Parameter(Mandatory)][string]$Message)
  throw "PROVISIONING POLICY VIOLATION: $Message"
}

# --- C/C++ comment + string handling ---------------------------------------
# Return source with comments neutralised (replaced by whitespace, newlines kept
# so line/index math stays stable). With -BlankStrings the interior of every
# string/char literal is also blanked (quotes preserved) so brace matching and
# identifier/API scans are never fooled by text inside a literal (e.g. a URL such
# as "http://192.168.4.1/" or a harmless word inside a message).
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
    # comment: keep newlines, blank everything else
    return [regex]::Replace($m.Value, "[^\r\n]", " ")
  }
  return $rx.Replace($Text, $evaluator)
}

# Extract the balanced { ... } body of the first function whose signature matches
# $SignaturePattern. Operates on skeleton text (strings blanked) so braces inside
# string literals cannot unbalance the match. Returns $null if not found.
function Get-FunctionBody {
  param(
    [Parameter(Mandatory)][string]$Skeleton,
    [Parameter(Mandatory)][string]$SignaturePattern
  )
  $m = [regex]::Match($Skeleton, $SignaturePattern)
  if (-not $m.Success) { return $null }
  $open = $Skeleton.IndexOf('{', $m.Index)
  if ($open -lt 0) { return $null }
  $depth = 0
  for ($j = $open; $j -lt $Skeleton.Length; $j++) {
    $ch = $Skeleton[$j]
    if ($ch -eq '{') { $depth++ }
    elseif ($ch -eq '}') {
      $depth--
      if ($depth -eq 0) { return $Skeleton.Substring($open, $j - $open + 1) }
    }
  }
  return $null
}

# Enumerate production source files under src/ and include/.
function Get-ProdSourceFiles {
  param([Parameter(Mandatory)][string]$Root)
  $files = @()
  foreach ($sub in @("src", "include")) {
    $dir = Join-Path $Root $sub
    if (Test-Path -LiteralPath $dir) {
      $files += Get-ChildItem -LiteralPath $dir -Recurse -File -Include *.cpp, *.cc, *.cxx, *.h, *.hpp
    }
  }
  return $files
}

# Build a one-shot index: relative path -> { Raw ; NoComments ; Skeleton }.
function Build-SourceIndex {
  param([Parameter(Mandatory)][string]$Root)
  $index = @{}
  foreach ($file in (Get-ProdSourceFiles -Root $Root)) {
    $raw = Get-Content -Raw -LiteralPath $file.FullName
    if ($null -eq $raw) { $raw = "" }
    $rel = $file.FullName.Substring($Root.Length).TrimStart('\', '/')
    $index[$rel] = [pscustomobject]@{
      Rel        = $rel
      Full       = $file.FullName
      Raw        = $raw
      NoComments = (Get-CodeSkeleton -Text $raw)
      Skeleton   = (Get-CodeSkeleton -Text $raw -BlankStrings)
    }
  }
  return $index
}

function Get-Indexed {
  param([Parameter(Mandatory)][hashtable]$Index, [Parameter(Mandatory)][string]$Rel)
  if (-not $Index.ContainsKey($Rel)) {
    Fail "expected production file is missing: $Rel"
  }
  return $Index[$Rel]
}

# Find every regex match across the index (default: on the string-blanked
# skeleton). Always returns an array of objects with Rel/Index/Value.
function Find-Across {
  param(
    [Parameter(Mandatory)][hashtable]$Index,
    [Parameter(Mandatory)][string]$Pattern,
    [ValidateSet("Skeleton", "NoComments", "Raw")][string]$On = "Skeleton"
  )
  $hits = New-Object System.Collections.ArrayList
  foreach ($key in $Index.Keys) {
    $text = $Index[$key].$On
    foreach ($m in [regex]::Matches($text, $Pattern)) {
      [void]$hits.Add([pscustomobject]@{ Rel = $key; Index = $m.Index; Value = $m.Value })
    }
  }
  return $hits.ToArray()
}

function Rels {
  param($Hits)
  return @($Hits | ForEach-Object { $_.Rel } | Select-Object -Unique)
}

# ===========================================================================
# The gate itself.
# ===========================================================================
function Invoke-ProvisioningGate {
  param(
    [Parameter(Mandatory)][string]$Root,
    [switch]$Quiet
  )

  function Ok { param([string]$m) if (-not $Quiet) { Write-Host "  OK: $m" } }
  function Section { param([string]$m) if (-not $Quiet) { Write-Host $m -ForegroundColor Cyan } }

  $index = Build-SourceIndex -Root $Root

  $routerRel  = "src\core\http_router.cpp"
  $portalRel  = "src\services\config_portal.cpp"
  $credsRel   = "src\services\wifi_credentials.cpp"
  $setupRel   = "src\services\wifi_setup.cpp"
  $markerRel  = "src\services\provision_marker.cpp"
  $identRel   = "src\services\device_identity.cpp"
  $mainRel    = "src\main.cpp"
  $displayRel = "src\ui\radar_display.cpp"

  # -----------------------------------------------------------------------
  # Invariant 1: no WiFiManager / OTA / mDNS / broad WebServer surface.
  # -----------------------------------------------------------------------
  Section "[1] No WiFiManager / OTA / mDNS / WebServer surface"

  $iniPath = Join-Path $Root "platformio.ini"
  if (-not (Test-Path -LiteralPath $iniPath)) { Fail "platformio.ini not found at $iniPath" }
  $iniRaw = Get-Content -Raw -LiteralPath $iniPath
  $iniCode = [regex]::Replace($iniRaw, "(?m);.*$", "")
  if ($iniCode -match "(?i)WiFiManager") {
    Fail "platformio.ini references a WiFiManager dependency (must never be a lib_deps entry)."
  }
  Ok "platformio.ini declares no WiFiManager dependency."

  $forbidden = @(
    @{ Name = "WiFiManager";      Pattern = "\bWiFiManager\b" },
    @{ Name = "WebServer";        Pattern = "\bWebServer\b" },
    @{ Name = "HTTPUpdateServer"; Pattern = "\bHTTPUpdateServer\b" },
    @{ Name = "ArduinoOTA";       Pattern = "\bArduinoOTA\b" },
    @{ Name = "ElegantOTA";       Pattern = "\bElegantOTA\b" },
    @{ Name = "ESPmDNS";          Pattern = "\bESPmDNS\b" },
    @{ Name = "MDNS";             Pattern = "\bMDNS\b" }
  )
  foreach ($f in $forbidden) {
    $hits = @(Find-Across -Index $index -Pattern $f.Pattern -On "Skeleton")
    if ($hits.Count -gt 0) {
      Fail "forbidden provisioning surface '$($f.Name)' used in production code: $((Rels $hits) -join ', ')"
    }
  }
  Ok "No WiFiManager/WebServer/HTTPUpdateServer/ArduinoOTA/ElegantOTA/ESPmDNS/MDNS in production code."

  # -----------------------------------------------------------------------
  # Invariant 2: closed captive route set in http_router.cpp.
  # -----------------------------------------------------------------------
  Section "[2] Closed captive route set (http_router.cpp)"

  $router = Get-Indexed -Index $index -Rel $routerRel
  $routerNc = $router.NoComments

  $routesBlock = [regex]::Match($routerNc, "(?s)kRoutes\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;")
  if (-not $routesBlock.Success) { Fail "could not locate the kRoutes[] allow-list in $routerRel" }
  $routesBody = $routesBlock.Groups[1].Value

  $expectedRoutes = @(
    "GET /", "POST /save", "GET /generate_204", "GET /gen_204",
    "GET /hotspot-detect.html", "GET /ncsi.txt", "GET /connecttest.txt",
    "GET /canonical.html", "GET /success.txt", "GET /library/test/success.html"
  )
  $methodMap = @{ "Get" = "GET"; "Post" = "POST"; "Put" = "PUT"; "Delete" = "DELETE"; "Head" = "HEAD" }
  $actualRoutes = @()
  foreach ($m in [regex]::Matches($routesBody, "\{\s*HttpMethod::(\w+)\s*,\s*`"([^`"]*)`"")) {
    $meth = $m.Groups[1].Value
    if ($methodMap.ContainsKey($meth)) { $meth = $methodMap[$meth] }
    $actualRoutes += "$meth $($m.Groups[2].Value)"
  }
  if ($actualRoutes.Count -ne $expectedRoutes.Count) {
    Fail ("kRoutes must contain EXACTLY {0} method/path pairs; found {1}: [{2}]" -f `
        $expectedRoutes.Count, $actualRoutes.Count, ($actualRoutes -join "; "))
  }
  $missing = @($expectedRoutes | Where-Object { $actualRoutes -notcontains $_ })
  $extra = @($actualRoutes | Where-Object { $expectedRoutes -notcontains $_ })
  if ($missing.Count -gt 0) { Fail "kRoutes is missing required route(s): $($missing -join '; ')" }
  if ($extra.Count -gt 0) { Fail "kRoutes contains unexpected route(s): $($extra -join '; ')" }
  if ($routesBody -match "\*") { Fail "kRoutes contains a wildcard '*' route path (only exact routes allowed)." }
  Ok "kRoutes is exactly the 10 allowed GET /, POST /save, and 8 captive-probe pairs."

  if ($router.Skeleton -notmatch "exactEqual\s*\(\s*path") {
    Fail "http_router no longer matches routes via exactEqual(path,...): a prefix/wildcard match may have been introduced."
  }
  if ($router.Skeleton -match "\bstrncmp\b" -or $router.Skeleton -match "startsWith" -or $router.Skeleton -match "\bstrstr\b") {
    Fail "http_router uses a prefix/substring match primitive (strncmp/startsWith/strstr): routes must be exact."
  }
  Ok "Route matching remains exact (no wildcard/prefix/substring matcher)."

  $denyBlock = [regex]::Match($routerNc, "(?s)kDenyPaths\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;")
  if (-not $denyBlock.Success) { Fail "could not locate the kDenyPaths deny-list in $routerRel" }
  $denyBody = $denyBlock.Groups[1].Value
  $requiredDeny = @("/update", "/u", "/erase", "/reset", "/reboot", "/restart", "/info", "/fwlink", "/setwifisave")
  foreach ($d in $requiredDeny) {
    if ($denyBody -notmatch ("`"" + [regex]::Escape($d) + "`"")) {
      Fail "kDenyPaths no longer explicitly denies '$d'."
    }
  }
  Ok "kDenyPaths still explicitly denies /update, /u, /erase, /reset, /reboot, /restart, /info, /fwlink, /setwifisave."

  # -----------------------------------------------------------------------
  # Invariant 3: no permanent / general listener.
  # -----------------------------------------------------------------------
  Section "[3] Single AP-bound listener; no general listener"

  $serverDecls = @(Find-Across -Index $index -Pattern "\bWiFiServer\s+\w+\s*[\(;]" -On "Skeleton")
  if ($serverDecls.Count -ne 1) {
    Fail "expected EXACTLY one production WiFiServer declaration; found $($serverDecls.Count): $((Rels $serverDecls) -join ', ')"
  }
  if ($serverDecls[0].Rel -ne $portalRel) {
    Fail "the single WiFiServer declaration must live in $portalRel, not $($serverDecls[0].Rel)."
  }
  Ok "Exactly one WiFiServer declaration, in config_portal.cpp."

  $portal = Get-Indexed -Index $index -Rel $portalRel
  $declStart = $portal.NoComments.IndexOf("WiFiServer s_server")
  if ($declStart -lt 0) { Fail "could not locate the WiFiServer s_server declaration in $portalRel." }
  $declEnd = $portal.NoComments.IndexOf(";", $declStart)
  $declText = $portal.NoComments.Substring($declStart, $declEnd - $declStart + 1)
  if ($declText -notmatch "kPortalIpOctets") {
    Fail "the WiFiServer is not bound to the fixed portal IP (config::kPortalIpOctets)."
  }
  if ($declText -notmatch ",\s*1\s*\)\s*;") {
    Fail "the WiFiServer is not constructed with a one-client backlog / max_clients=1."
  }
  Ok "WiFiServer is bound to the fixed portal IP with a one-client backlog."

  $dnsDecls = @(Find-Across -Index $index -Pattern "\bDNSServer\s+\w+\s*[\(;]" -On "Skeleton")
  if ($dnsDecls.Count -ne 1) {
    Fail "expected EXACTLY one production DNSServer declaration; found $($dnsDecls.Count): $((Rels $dnsDecls) -join ', ')"
  }
  if ($dnsDecls[0].Rel -ne $portalRel) {
    Fail "the single DNSServer declaration must live in $portalRel, not $($dnsDecls[0].Rel)."
  }
  Ok "Exactly one DNSServer declaration, in config_portal.cpp."

  $startCalls = @(Find-Across -Index $index -Pattern "services::portal::start\s*\(" -On "Skeleton")
  if ($startCalls.Count -ne 1) {
    Fail "expected EXACTLY one production call to services::portal::start; found $($startCalls.Count): $((Rels $startCalls) -join ', ')"
  }
  if ($startCalls[0].Rel -ne $setupRel) {
    Fail "the single services::portal::start call must come from $setupRel, not $($startCalls[0].Rel)."
  }
  Ok "Exactly one services::portal::start caller, in wifi_setup.cpp."

  $main = Get-Indexed -Index $index -Rel $mainRel
  foreach ($banned in @("WiFiServer", "DNSServer", "services::portal::start")) {
    if ($main.Skeleton -match [regex]::Escape($banned)) {
      Fail "main.cpp must not instantiate or start a server; found '$banned'."
    }
  }
  Ok "main.cpp instantiates/starts no server."

  $stopBody = Get-FunctionBody -Skeleton $portal.Skeleton -SignaturePattern "void\s+stop\s*\(\s*\)"
  if ($null -eq $stopBody) { Fail "could not locate config_portal stop() in $portalRel." }
  if ($stopBody -notmatch "s_server\.end\s*\(") { Fail "config_portal::stop must call s_server.end()." }
  if ($stopBody -notmatch "s_dns\.stop\s*\(") { Fail "config_portal::stop must call s_dns.stop()." }
  Ok "config_portal::stop calls s_server.end() and s_dns.stop()."

  # -----------------------------------------------------------------------
  # Invariant 4: no secret logging; secureZero on owned workspaces.
  # -----------------------------------------------------------------------
  Section "[4] No secret logging; secureZero of workspaces"

  $provisioningFiles = @($portalRel, $credsRel, $setupRel, $markerRel, $identRel,
    "src\core\txn_marker.cpp", "src\core\location_record.cpp")
  $logApiPattern = "(?:Serial\s*\.\s*(?:print|println|printf|write)|(?<![A-Za-z0-9_])printf|(?<![A-Za-z0-9_])log_[eviwd]|(?<![A-Za-z0-9_])ets_printf|(?<![A-Za-z0-9_])ESP_LOG[EWIVD])\s*\("
  $secretTokenPattern = "(?i)(password|passphrase|\bpsk\b|csrf|token|candidate|secret)"
  foreach ($rel in $provisioningFiles) {
    if (-not $index.ContainsKey($rel)) { continue }
    $sk = $index[$rel].Skeleton
    foreach ($m in [regex]::Matches($sk, $logApiPattern)) {
      $semi = $sk.IndexOf(";", $m.Index)
      if ($semi -lt 0) { $semi = [Math]::Min($m.Index + 400, $sk.Length - 1) }
      $stmt = $sk.Substring($m.Index, $semi - $m.Index + 1)
      if ($stmt -match $secretTokenPattern) {
        Fail "secret-bearing field passed to a logging/serial API in ${rel}: '$($stmt.Trim())'"
      }
    }
  }
  Ok "No provisioning file logs a password/PSK/CSRF/token/candidate field."

  foreach ($rel in @($portalRel, $credsRel)) {
    $sk = (Get-Indexed -Index $index -Rel $rel).Skeleton
    if ($sk -notmatch "secureZero\s*\(") {
      Fail "$rel must call secureZero() on its owned request/response/config workspaces."
    }
  }
  Ok "config_portal.cpp and wifi_credentials.cpp secureZero their owned workspaces."

  # -----------------------------------------------------------------------
  # Invariant 5: RAM/FLASH transaction policy.
  # -----------------------------------------------------------------------
  Section "[5] RAM/FLASH storage transaction policy"

  $storageCalls = @(Find-Across -Index $index -Pattern "esp_wifi_set_storage\s*\(" -On "Skeleton")
  $badStorage = @($storageCalls | Where-Object { $_.Rel -ne $credsRel })
  if ($badStorage.Count -gt 0) {
    Fail "esp_wifi_set_storage may only be called from $credsRel; also found in: $((Rels $badStorage) -join ', ')"
  }
  if ($storageCalls.Count -lt 1) { Fail "no esp_wifi_set_storage call found in $credsRel (expected the RAM/FLASH selectors)." }
  Ok "esp_wifi_set_storage is confined to wifi_credentials.cpp."

  $setup = Get-Indexed -Index $index -Rel $setupRel
  if ($setup.Skeleton -notmatch "WiFi\.persistent\s*\(\s*false\s*\)") {
    Fail "runtime must declare WiFi.persistent(false) in wifi_setup.cpp."
  }
  Ok "Runtime declares WiFi.persistent(false)."

  $flashHits = @(Find-Across -Index $index -Pattern "setStorageFlash\s*\(\s*\)" -On "Skeleton")
  $flashCallers = @()
  foreach ($h in $flashHits) {
    $text = $index[$h.Rel].Skeleton
    $back = $text.Substring([Math]::Max(0, $h.Index - 20), [Math]::Min(20, $h.Index)).Trim()
    if ($back -match "bool$") { continue }  # the definition itself
    $flashCallers += $h
  }
  $callerFiles = @($flashCallers | ForEach-Object { $_.Rel } | Select-Object -Unique)
  $allowedFlash = @($setupRel, $credsRel)
  foreach ($cf in $callerFiles) {
    if ($allowedFlash -notcontains $cf) {
      Fail "setStorageFlash() called from an unauthorized file: $cf (only the commit path + verified erase helper may select FLASH)."
    }
  }
  $creds = Get-Indexed -Index $index -Rel $credsRel
  $commitBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+executeCommit\s*\(\s*\)"
  if ($null -eq $commitBody) { Fail "could not locate executeCommit() in $setupRel." }
  if ($commitBody -notmatch "setStorageFlash\s*\(") { Fail "executeCommit() must select FLASH via setStorageFlash() in its critical section." }
  $eraseStaBody = Get-FunctionBody -Skeleton $creds.Skeleton -SignaturePattern "bool\s+eraseSta\s*\(\s*\)"
  if ($null -eq $eraseStaBody) { Fail "could not locate eraseSta() in $credsRel." }
  if ($eraseStaBody -notmatch "setStorageFlash\s*\(") { Fail "eraseSta() must select FLASH via setStorageFlash()." }
  Ok "setStorageFlash() callers are only executeCommit (commit path) and eraseSta (verified erase helper)."

  $beginBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+executeBeginCandidate\s*\(\s*\)"
  if ($null -eq $beginBody) { Fail "could not locate executeBeginCandidate() in $setupRel." }
  if ($beginBody -notmatch "(ensureStorageRam|setStorageRam)\s*\(") {
    Fail "executeBeginCandidate() must select RAM storage before staging a candidate."
  }
  $restoreBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+executeRestore\s*\(\s*\)"
  if ($null -eq $restoreBody) { Fail "could not locate executeRestore() in $setupRel." }
  if ($restoreBody -notmatch "setStorageRam\s*\(") {
    Fail "executeRestore() must apply the old config to RAM (setStorageRam)."
  }
  if ($eraseStaBody -notmatch "setStorageRam\s*\(") {
    Fail "eraseSta() must restore RAM storage after the FLASH erase."
  }
  Ok "Candidate begin/restore require RAM; eraseSta restores RAM."

  $idxCommitMarker = [regex]::Match($commitBody, "CommitInProgress").Index
  $idxCommitFlash = [regex]::Match($commitBody, "setStorageFlash\s*\(").Index
  if ($idxCommitMarker -lt 0 -or $idxCommitFlash -lt 0 -or $idxCommitMarker -ge $idxCommitFlash) {
    Fail "executeCommit() must persist the CommitInProgress marker BEFORE selecting FLASH."
  }
  if ($commitBody -notmatch "if\s*\(\s*!\s*services::provision_marker::writeVerified\s*\(\s*core::TxnMarkerState::None") {
    Fail "executeCommit() must CHECK the marker clear (writeVerified(...None)) and fail closed if it cannot be verified."
  }
  $eraseBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+executeFactoryErase\s*\(\s*\)"
  if ($null -eq $eraseBody) { Fail "could not locate executeFactoryErase() in $setupRel." }
  $mErasePending = [regex]::Match($eraseBody, "ErasePending")
  $mEraseSta = [regex]::Match($eraseBody, "eraseSta\s*\(")
  if (-not $mErasePending.Success -or -not $mEraseSta.Success -or $mErasePending.Index -ge $mEraseSta.Index) {
    Fail "executeFactoryErase() must persist the ErasePending marker BEFORE erasing (eraseSta)."
  }
  if ($eraseBody -notmatch "marker_cleared\s*=" -or $eraseBody -notmatch "writeVerified\s*\(\s*core::TxnMarkerState::None") {
    Fail "executeFactoryErase() must gate completion on a checked marker clear (writeVerified(...None))."
  }
  Ok "Durable markers precede commit/erase in source order and their clears are checked."

  # (Coordinator hardening) A recently-fixed defect had executeFactoryErase() CALL
  # terminalNetworkOff() but IGNORE its result and erase anyway. Seeing the function
  # name is NOT enough: require an explicit CHECKED guard equivalent to
  #   if (!terminalNetworkOff()) { ...; return; }
  # that (a) sits AFTER the ErasePending marker request and BEFORE the first
  # eraseSta(), and (b) whose failure branch diverts into enterEraseIncomplete() (or
  # otherwise returns) BEFORE any erase, so a failed teardown can never fall through.
  $mNetGuard = [regex]::Match($eraseBody, "if\s*\(\s*!\s*terminalNetworkOff\s*\(\s*\)\s*\)\s*\{")
  if (-not $mNetGuard.Success) {
    Fail "executeFactoryErase() must CHECK terminalNetworkOff() with a guard equivalent to 'if (!terminalNetworkOff()) { ... return; }'; a bare, result-ignoring terminalNetworkOff() would still erase after a failed teardown."
  }
  if ($mNetGuard.Index -le $mErasePending.Index -or $mNetGuard.Index -ge $mEraseSta.Index) {
    Fail "executeFactoryErase()'s checked terminalNetworkOff() guard must sit AFTER the ErasePending marker request and BEFORE the first eraseSta()."
  }
  # The guard's failure branch must divert away from erase BEFORE any eraseSta():
  # balance-match the guard's own { ... } block and require it to enter
  # enterEraseIncomplete() (or at least return), and to close before the erase.
  $guardOpen = $eraseBody.IndexOf('{', $mNetGuard.Index)
  $guardDepth = 0
  $guardClose = -1
  for ($j = $guardOpen; $j -lt $eraseBody.Length; $j++) {
    $gch = $eraseBody[$j]
    if ($gch -eq '{') { $guardDepth++ }
    elseif ($gch -eq '}') { $guardDepth--; if ($guardDepth -eq 0) { $guardClose = $j; break } }
  }
  if ($guardClose -lt 0 -or $guardClose -ge $mEraseSta.Index) {
    Fail "executeFactoryErase()'s terminalNetworkOff() guard block must close BEFORE the first eraseSta() (the failed-teardown branch must not fall through to the erase)."
  }
  $guardBlock = $eraseBody.Substring($guardOpen, $guardClose - $guardOpen + 1)
  if ($guardBlock -notmatch "enterEraseIncomplete\s*\(" -and $guardBlock -notmatch "\breturn\b") {
    Fail "executeFactoryErase()'s failed terminalNetworkOff() branch must enter enterEraseIncomplete() (or return) before erasing anything."
  }
  # enterEraseIncomplete() must itself re-CHECK terminal network-off before freezing,
  # so the fail-closed terminal state also proves every network surface is down.
  $incompleteBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+enterEraseIncomplete\s*\(\s*\)"
  if ($null -eq $incompleteBody) { Fail "could not locate enterEraseIncomplete() in $setupRel." }
  if ($incompleteBody -notmatch "terminalNetworkOff\s*\(") {
    Fail "enterEraseIncomplete() must call terminalNetworkOff() to re-prove every network surface is off before freezing."
  }
  Ok "executeFactoryErase() checks terminalNetworkOff() (guarded, pre-erase) and diverts to enterEraseIncomplete(), which re-checks network-off."

  # (Coordinator hardening) Commit ambiguity: executeCommit() must SET its
  # flash-touched / apply-attempted latch BEFORE calling applySta(s_candidate_config),
  # so an apply that returns false (but may still have written NVS) stays rollback-
  # gated instead of being treated as a no-op.
  $mCommitLatch = [regex]::Match($commitBody, "apply_attempted\s*=\s*true")
  $mCommitApply = [regex]::Match($commitBody, "applySta\s*\(\s*s_candidate_config\s*\)")
  if (-not $mCommitLatch.Success -or -not $mCommitApply.Success -or $mCommitLatch.Index -ge $mCommitApply.Index) {
    Fail "executeCommit() must set its apply-attempted/flash-touched latch BEFORE calling applySta(s_candidate_config) so a false-returning apply stays rollback-gated."
  }
  Ok "executeCommit() latches apply-attempted before applySta(s_candidate_config) (false apply stays rollback-gated)."

  # -----------------------------------------------------------------------
  # Invariant 6: no insecure AP teardown regression.
  # -----------------------------------------------------------------------
  Section "[6] No insecure AP teardown"

  $apDisc = @(Find-Across -Index $index -Pattern "\bsoftAPdisconnect\s*\(" -On "Skeleton")
  if ($apDisc.Count -gt 0) {
    Fail "softAPdisconnect() must not appear in production; found in: $((Rels $apDisc) -join ', ')"
  }
  $softAp = @(Find-Across -Index $index -Pattern "WiFi\.softAP\s*\(" -On "Skeleton")
  if ($softAp.Count -gt 0) {
    Fail "WiFi.softAP( must not appear in production (would risk an open/default AP); found in: $((Rels $softAp) -join ', ')"
  }
  Ok "No softAPdisconnect / WiFi.softAP( in production."

  $eraseApTrue = @(Find-Across -Index $index -Pattern "eraseap\s*=\s*true" -On "Skeleton")
  if ($eraseApTrue.Count -gt 0) {
    Fail "an eraseap=true call was found in: $((Rels $eraseApTrue) -join ', ') (the project must never erase AP config that way)."
  }
  Ok "No eraseap=true call in production."

  $listenerBody = Get-FunctionBody -Skeleton $setup.Skeleton -SignaturePattern "void\s+executeStartListener\s*\(\s*\)"
  if ($null -eq $listenerBody) { Fail "could not locate executeStartListener() in $setupRel." }
  $iStop = [regex]::Match($listenerBody, "esp_wifi_stop\s*\(")
  $iMode = [regex]::Match($listenerBody, "esp_wifi_set_mode\s*\(\s*WIFI_MODE_AP")
  $iCfg = [regex]::Match($listenerBody, "esp_wifi_set_config\s*\(\s*WIFI_IF_AP")
  $iStart = [regex]::Match($listenerBody, "esp_wifi_start\s*\(")
  if (-not ($iStop.Success -and $iMode.Success -and $iCfg.Success -and $iStart.Success)) {
    Fail "secure AP creation must call esp_wifi_stop, esp_wifi_set_mode(AP), esp_wifi_set_config(AP) and esp_wifi_start."
  }
  if (-not ($iStop.Index -lt $iMode.Index -and $iMode.Index -lt $iCfg.Index -and $iCfg.Index -lt $iStart.Index)) {
    Fail "secure AP creation order regressed; required order is stop -> AP mode -> secured config -> start."
  }
  Ok "Secure AP creation is raw stop -> AP mode -> secured config -> start."

  # -----------------------------------------------------------------------
  # Invariant 7: one framebuffer.
  # -----------------------------------------------------------------------
  Section "[7] Single framebuffer sprite"

  $spriteDecls = @(Find-Across -Index $index -Pattern "\bLGFX_Sprite\s+\w+" -On "Skeleton")
  if ($spriteDecls.Count -ne 1) {
    Fail "expected EXACTLY one full-frame LGFX_Sprite declaration; found $($spriteDecls.Count): $((Rels $spriteDecls) -join ', ')"
  }
  if ($spriteDecls[0].Rel -ne $displayRel) {
    Fail "the single LGFX_Sprite must live in $displayRel, not $($spriteDecls[0].Rel)."
  }
  $createSprite = @(Find-Across -Index $index -Pattern "\.createSprite\s*\(" -On "Skeleton")
  if ($createSprite.Count -ne 1) {
    Fail "expected EXACTLY one createSprite() call; found $($createSprite.Count): $((Rels $createSprite) -join ', ')"
  }
  $rawFb = @(Find-Across -Index $index -Pattern "(?:heap_caps_malloc|ps_malloc|malloc|calloc)\s*\([^;]*240\s*\*\s*240" -On "Skeleton")
  if ($rawFb.Count -gt 0) {
    Fail "a second 240x240 raw pixel-buffer allocation was found in: $((Rels $rawFb) -join ', ')."
  }
  Ok "Exactly one LGFX_Sprite + one createSprite; no second 240x240 pixel allocation."

  # -----------------------------------------------------------------------
  # Invariant 8: bounded portal.
  # -----------------------------------------------------------------------
  Section "[8] Bounded config portal"

  $portalSk = $portal.Skeleton
  if ($portalSk -notmatch "httpLimitsValid\s*\(" -or $portalSk -notmatch "kPortalHttpLimits") {
    Fail "config_portal must validate fixed HTTP limits (httpLimitsValid / kPortalHttpLimits)."
  }
  foreach ($banned in @("\bString\b", "\bmalloc\b", "\bcalloc\b", "\brealloc\b", "\bnew\b")) {
    if ($portalSk -match $banned) {
      Fail "config_portal must not use dynamic allocation / String; found pattern '$banned'."
    }
  }
  if ($portalSk -notmatch "\blwip_recv\b" -or $portalSk -notmatch "\blwip_send\b") {
    Fail "config_portal must use raw lwip_recv / lwip_send on the socket fd."
  }
  $startBody = Get-FunctionBody -Skeleton $portalSk -SignaturePattern "bool\s+start\s*\("
  if ($null -eq $startBody) { Fail "could not locate config_portal start() in $portalRel." }
  if ($startBody -notmatch "s_server\.end\s*\(") {
    Fail "config_portal start() must call s_server.end() on a failed bring-up."
  }
  Ok "Portal has fixed limits, no String/malloc/new, lwip_recv/lwip_send, and a start-failure end()."

  if (-not $Quiet) { Write-Host "All provisioning policy invariants verified." -ForegroundColor Green }
}

# ===========================================================================
# Negative tamper self-test.
# ===========================================================================
function Invoke-TamperSelfTest {
  param([Parameter(Mandatory)][string]$Root)

  Write-Host "=== Negative tamper self-test (proves the gate REJECTS regressions) ===" -ForegroundColor Cyan

  $tempRoot = Join-Path $Root (".provgate-selftest-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
  try {
    foreach ($sub in @("src", "include")) {
      $srcDir = Join-Path $Root $sub
      if (Test-Path -LiteralPath $srcDir) {
        Copy-Item -LiteralPath $srcDir -Destination (Join-Path $tempRoot $sub) -Recurse -Force
      }
    }
    Copy-Item -LiteralPath (Join-Path $Root "platformio.ini") -Destination (Join-Path $tempRoot "platformio.ini") -Force

    Invoke-ProvisioningGate -Root $tempRoot -Quiet
    Write-Host "  OK (baseline): the untampered mirror passes the gate." -ForegroundColor Green

    $cases = @(
      @{ Name = "re-add WiFiManager dependency";     Rel = "platformio.ini";
        Mutate = { param($t) $t -replace '(bblanchon/ArduinoJson@7\.4\.3)', "`$1`n  tzapu/WiFiManager@2.0.17" } },
      @{ Name = "add a forbidden /update route";      Rel = "src\core\http_router.cpp";
        Mutate = { param($t) $t -replace '(\{HttpMethod::Get, "/", HttpRoute::Root\},)', "`$1`n    {HttpMethod::Get, `"/update`", HttpRoute::Root}," } },
      @{ Name = "add an extra captive route";         Rel = "src\core\http_router.cpp";
        Mutate = { param($t) $t -replace '(\{HttpMethod::Get, "/", HttpRoute::Root\},)', "`$1`n    {HttpMethod::Get, `"/extra`", HttpRoute::CaptiveNcsi}," } },
      @{ Name = "log a secret (CSRF) to Serial";      Rel = "src\services\config_portal.cpp";
        Mutate = { param($t) $t -replace '(void wipeWorkspaces\(\) \{)', "`$1`n  Serial.printf(`"leak csrf=%s`", s_content.csrf_token);" } },
      @{ Name = "add a second framebuffer sprite";    Rel = "src\ui\radar_display.cpp";
        Mutate = { param($t) $t -replace '(LGFX_Sprite s_frame\(&tft\);)', "`$1`nLGFX_Sprite s_frame_shadow(&tft);" } },
      @{ Name = "insecure softAPdisconnect(true)";    Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace '(void executeDisconnectSta\(\) \{)', "`$1`n  WiFi.softAPdisconnect(true);" } },
      @{ Name = "esp_wifi_set_storage outside creds"; Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace '(void executeDisconnectSta\(\) \{)', "`$1`n  esp_wifi_set_storage(WIFI_STORAGE_FLASH);" } },
      @{ Name = "bypass checked terminalNetworkOff() guard (ignored teardown)"; Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace '(?s)if \(!terminalNetworkOff\(\)\) \{.*?return;\s*\}', "terminalNetworkOff();" } },
      @{ Name = "commit latches apply-attempted AFTER applySta (ambiguity)"; Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace '(?s)apply_attempted = true;[^\r\n]*\r?\n(\s*)committed = (services::wifi_creds::applySta\(s_candidate_config\);)', "committed = `$2`n`$1apply_attempted = true;" } }
    )

    foreach ($case in $cases) {
      $targetPath = Join-Path $tempRoot $case.Rel
      if (-not (Test-Path -LiteralPath $targetPath)) { Fail "self-test target missing: $($case.Rel)" }
      $original = Get-Content -Raw -LiteralPath $targetPath
      $mutated = & $case.Mutate $original
      if ($mutated -eq $original) {
        Fail "self-test '$($case.Name)': mutation did not change $($case.Rel) (anchor not found)."
      }
      Set-Content -LiteralPath $targetPath -Value $mutated -NoNewline -Encoding utf8
      try {
        $detected = $false
        try {
          Invoke-ProvisioningGate -Root $tempRoot -Quiet
        } catch {
          $detected = $true
        }
        if (-not $detected) {
          throw "SELF-TEST FAILURE: tamper '$($case.Name)' was NOT rejected by the gate."
        }
        Write-Host "  OK (rejected): $($case.Name)" -ForegroundColor Green
      } finally {
        Set-Content -LiteralPath $targetPath -Value $original -NoNewline -Encoding utf8
      }
    }

    Invoke-ProvisioningGate -Root $tempRoot -Quiet
    Write-Host "  OK (post-restore): the mirror passes again after all mutations reverted." -ForegroundColor Green
    Write-Host "Tamper self-test passed: the gate rejects every representative regression." -ForegroundColor Green
  } finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}

# ===========================================================================
# Entry point.
# ===========================================================================
if ($SelfTest) {
  Invoke-TamperSelfTest -Root $ProjectRoot
} else {
  Write-Host "=== Phase 8 provisioning policy gate ($ProjectRoot) ===" -ForegroundColor Cyan
  Invoke-ProvisioningGate -Root $ProjectRoot
}
