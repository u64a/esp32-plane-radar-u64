[CmdletBinding()]
param(
  # Root of the tree to verify. Defaults to the repository root. Negative-proof
  # harnesses point this at a tampered copy of src/ to confirm the gate fails.
  [string]$ProjectRoot,
  # Fail if the earliest pinned root expires within this many days (maintenance
  # horizon). Deterministic: compared against the machine clock only, never the
  # network.
  [int]$MinRootValidityDays = 365,
  # Optional, NON-GATING live TLS advisory against the ADS-B endpoint. Never
  # changes the exit code and never mutates the committed trust bundle.
  [switch]$LiveAdvisory
)

# Deterministic, OFFLINE verification of the committed ADS-B CA trust bundle and
# the production TLS + trusted-time clock wiring. This gate NEVER downloads a
# certificate, never mutates the committed bundle, and needs no compiler or
# network -- so it is safe to run in any build. It proves, from source alone:
#   1. The committed bundle (src/services/adsb_ca_bundle.cpp) parses to EXACTLY
#      the four pinned roots, each IDENTIFIED by its expected SHA-256 fingerprint
#      (the authoritative pin), with a matching subject, self-ISSUED naming
#      (subject == issuer), and sane validity metadata. This checks self-issued
#      NAMING and the exact DER fingerprint -- it does NOT perform a cryptographic
#      self-signature verification (the SHA-256 pin is the identity guarantee).
#   2. The earliest root's expiry is comfortably beyond a maintenance horizon.
#   3. Production source (src/ AND include/) contains NO setInsecure() call.
#   4. The pinned CA bundle is actually passed through the IP+host connect
#      overload, and the client hands kAdsbCaBundle to the transport.
#   5. The trusted-time adapter derives UTC only from the accepted-sample
#      monotonic clock -- no raw time(nullptr)/time(NULL) in the trust decisions.
#   6. main passes the CA-authenticated peer-notBefore field (FetchResult.
#      authenticated_cert_not_before_unix) into the persisted-floor service, and
#      the superseded SNTP-time ratchet call (noteVerifiedFetch) is gone.
#   7. The persisted-floor ratchet (timekeeper's noteVerifiedCertFloor) reads NO
#      derived/SNTP clock (nowUnix / derivedTrustedNowUnix / trusted_now) for its
#      NVS write: it persists only the CA-signed certificate notBefore, so an
#      unauthenticated NTP attacker can cause at most a non-persistent DoS and can
#      never poison NVS.
#   8. The ESP transport (espVerifyPeerCertValidity) still enforces certificate
#      dates over the FULL retained peer chain: it walks mbedtls_x509_crt::next and
#      feeds every node through the bounded Arduino-free accumulator
#      (certChainBegin / certChainAddNode / certChainFinalize), and the core caps
#      that walk with a fixed kMaxPeerChainLen. This catches a removed chain walk
#      or a leaf-only reversion (which the pinned no-HAVE_TIME_DATE build would let
#      accept an expired/not-yet-valid peer intermediate).
# Any failure throws (non-zero exit). Works under Windows PowerShell 5.1 and
# pwsh 7. The bundle parse is fully in-memory: no temporary files are created.

$ErrorActionPreference = "Stop"

if (-not $ProjectRoot) {
  $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$srcRoot = Join-Path $ProjectRoot "src"
$includeRoot = Join-Path $ProjectRoot "include"
$bundlePath = Join-Path $srcRoot "services\adsb_ca_bundle.cpp"
$transportPath = Join-Path $srcRoot "services\adsb_transport_esp.cpp"
$clientPath = Join-Path $srcRoot "services\adsb_client.cpp"
$timekeeperPath = Join-Path $srcRoot "services\timekeeper.cpp"
$certTimePath = Join-Path $srcRoot "core\cert_time.cpp"
$mainPath = Join-Path $srcRoot "main.cpp"

foreach ($p in @($bundlePath, $transportPath, $clientPath, $timekeeperPath, $certTimePath, $mainPath)) {
  if (-not (Test-Path $p)) {
    throw "Required source file not found: $p"
  }
}

# Pinned trust anchors: expected CN, organization substring, and the SHA-256
# fingerprint of the DER certificate (lower-case hex, no separators). These are
# the authoritative pins; changing the committed bytes without updating these
# (and vice versa) fails the gate.
$expectedRoots = @(
  [pscustomobject]@{
    Name = "ISRG Root X1"; Cn = "CN=ISRG Root X1"
    Org = "Internet Security Research Group"
    Sha256 = "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6"
  },
  [pscustomobject]@{
    Name = "ISRG Root X2"; Cn = "CN=ISRG Root X2"
    Org = "Internet Security Research Group"
    Sha256 = "69729b8e15a86efc177a57afb7171dfc64add28c2fca8cf1507e34453ccb1470"
  },
  [pscustomobject]@{
    Name = "GTS Root R1"; Cn = "CN=GTS Root R1"
    Org = "Google Trust Services"
    Sha256 = "d947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf"
  },
  [pscustomobject]@{
    Name = "GTS Root R4"; Cn = "CN=GTS Root R4"
    Org = "Google Trust Services"
    Sha256 = "349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d"
  }
)
$expectedCount = $expectedRoots.Count

# --- helpers ---------------------------------------------------------------

# SHA-256 of a byte array as lower-case hex. Explicit [byte[]] avoids the
# ComputeHash overload ambiguity and works on .NET Framework 4.x and .NET 5+.
function Get-Sha256Hex([byte[]]$bytes) {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $hash = $sha.ComputeHash([byte[]]$bytes)
  } finally {
    $sha.Dispose()
  }
  return (-join ($hash | ForEach-Object { $_.ToString("x2") }))
}

# Reconstruct the C string literal assigned to kAdsbCaBundle exactly as the
# compiler would, then split it into PEM certificate blocks and parse each into
# an X509 certificate object with its DER SHA-256. Fully in-memory. The raw text
# is used deliberately: the provenance comments carry no double-quote or ';', so
# the string-literal chunk scan skips them, while base64 payloads (which contain
# '//') are never mistaken for comments.
function Get-BundleCertificates([string]$path) {
  $raw = Get-Content -Raw $path
  $m = [regex]::Match($raw, "(?s)kAdsbCaBundle\s*\[\s*\]\s*=\s*(.*?);")
  if (-not $m.Success) {
    throw "Could not locate the kAdsbCaBundle string literal in $path"
  }
  $literal = $m.Groups[1].Value
  $chunks = [regex]::Matches($literal, '"((?:[^"\\]|\\.)*)"')
  if ($chunks.Count -eq 0) {
    throw "kAdsbCaBundle contains no string literal chunks in $path"
  }
  $pem = (-join ($chunks | ForEach-Object { $_.Groups[1].Value })) -replace '\\n', "`n"

  $blocks = [regex]::Matches(
    $pem, "(?s)-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----")
  $results = @()
  foreach ($b in $blocks) {
    $b64 = ($b.Value -replace "-----[^-]+-----", "") -replace "\s", ""
    try {
      $der = [System.Convert]::FromBase64String($b64)
    } catch {
      throw "A committed certificate block is not valid base64: $($_.Exception.Message)"
    }
    try {
      $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 `
        -ArgumentList (, [byte[]]$der)
    } catch {
      throw "A committed certificate block failed to parse as X.509: $($_.Exception.Message)"
    }
    $results += [pscustomobject]@{
      Subject   = $cert.Subject
      Issuer    = $cert.Issuer
      NotBefore = $cert.NotBefore.ToUniversalTime()
      NotAfter  = $cert.NotAfter.ToUniversalTime()
      Sha256    = Get-Sha256Hex ([byte[]]$cert.RawData)
      Cert      = $cert
    }
  }
  return , $results
}

# --- 1. Parse + verify the committed bundle --------------------------------

$certs = Get-BundleCertificates $bundlePath

if ($certs.Count -ne $expectedCount) {
  throw "CA bundle must contain exactly $expectedCount certificates; found $($certs.Count)."
}
Write-Host "OK: bundle contains exactly $expectedCount certificates."

$now = [DateTime]::UtcNow
$earliestNotAfter = $null

foreach ($want in $expectedRoots) {
  $match = $certs | Where-Object { $_.Sha256 -eq $want.Sha256 }
  if (-not $match) {
    throw "Pinned root '$($want.Name)' (SHA-256 $($want.Sha256)) is MISSING from the bundle."
  }
  if (@($match).Count -ne 1) {
    throw "Pinned root '$($want.Name)' appears more than once in the bundle."
  }
  $c = @($match)[0]
  if ($c.Subject -notmatch [regex]::Escape($want.Cn)) {
    throw "Root '$($want.Name)' subject mismatch: '$($c.Subject)' lacks '$($want.Cn)'."
  }
  if ($c.Subject -notmatch [regex]::Escape($want.Org)) {
    throw "Root '$($want.Name)' subject lacks organization '$($want.Org)': '$($c.Subject)'."
  }
  if ($c.Subject -ne $c.Issuer) {
    throw "Root '$($want.Name)' is not self-issued (subject != issuer)."
  }
  if ($c.NotBefore -ge $c.NotAfter) {
    throw "Root '$($want.Name)' has invalid validity metadata (notBefore >= notAfter)."
  }
  if ($now -lt $c.NotBefore) {
    throw "Root '$($want.Name)' is not yet valid (notBefore $($c.NotBefore.ToString('u')))."
  }
  if ($now -gt $c.NotAfter) {
    throw "Root '$($want.Name)' is already EXPIRED (notAfter $($c.NotAfter.ToString('u')))."
  }
  if ($null -eq $earliestNotAfter -or $c.NotAfter -lt $earliestNotAfter) {
    $earliestNotAfter = $c.NotAfter
  }
  Write-Host ("OK: {0}: self-issued naming, SHA-256 pin verified, valid {1} .. {2}." -f `
      $want.Name, $c.NotBefore.ToString("yyyy-MM-dd"), $c.NotAfter.ToString("yyyy-MM-dd"))
}

# No unexpected certificates (every committed fingerprint must be pinned).
$expectedHashes = $expectedRoots | ForEach-Object { $_.Sha256 }
foreach ($c in $certs) {
  if ($expectedHashes -notcontains $c.Sha256) {
    throw "Bundle contains an UNEXPECTED certificate (SHA-256 $($c.Sha256)): $($c.Subject)."
  }
}
Write-Host "OK: no unexpected certificates in the bundle."

# --- 2. Maintenance horizon -------------------------------------------------

$daysLeft = [math]::Floor(($earliestNotAfter - $now).TotalDays)
if ($daysLeft -lt $MinRootValidityDays) {
  throw ("Earliest root maintenance horizon too close: {0} days left (< {1}); " +
    "refresh the pinned roots.") -f $daysLeft, $MinRootValidityDays
}
Write-Host ("OK: earliest root expires {0} ({1} days out, >= {2} required)." -f `
    $earliestNotAfter.ToString("yyyy-MM-dd"), $daysLeft, $MinRootValidityDays)

# --- 3. No setInsecure() anywhere in production source (src/ AND include/) --
# A real WiFiClientSecure::setInsecure() is always a method call (obj.setInsecure(
# ...) or ptr->setInsecure(...)). Matching that call form on the raw text catches
# any genuine use while ignoring the prose that documents the no-insecure policy,
# and without the base64 '//' hazard of comment stripping. Both trees are scanned
# because a setInsecure() in a header (include/) would be just as fatal as one in
# a .cpp.

$sourceFiles = Get-ChildItem -Path @($srcRoot, $includeRoot) -Recurse `
  -Include *.cpp, *.h, *.hpp -File
$insecureHits = @()
foreach ($f in $sourceFiles) {
  $text = Get-Content -Raw $f.FullName
  if ($text -match "(?:\.|->)\s*setInsecure\s*\(") {
    $insecureHits += $f.FullName
  }
}
if ($insecureHits.Count -gt 0) {
  throw "Production source calls setInsecure(): $($insecureHits -join ', ')"
}
Write-Host ("OK: no setInsecure() call in production source (src/ + include/, " +
  "$($sourceFiles.Count) files scanned).")

# --- 4. CA actually passed through the IP+host connect overload ------------

$transportRaw = Get-Content -Raw $transportPath
# The verified connect must forward the ca_bundle parameter as the 4th argument
# of the IP+host overload (ip, port, host, CA, cert, key).
if ($transportRaw -notmatch "\.connect\s*\(\s*address\s*,\s*port\s*,\s*host\s*,\s*ca_bundle\s*,") {
  throw ("$transportPath does not pass ca_bundle as the 4th argument of the " +
    "IP+host connect overload (client.connect(address, port, host, ca_bundle, ...)).")
}
# And it must NOT pass a null CA (host followed immediately by nullptr/NULL).
if ($transportRaw -match "\.connect\s*\(\s*address\s*,\s*port\s*,\s*host\s*,\s*(?:nullptr|NULL)\s*,") {
  throw "$transportPath passes a NULL CA to the connect overload (insecure)."
}
Write-Host "OK: transport passes ca_bundle through the IP+host connect overload."

$clientRaw = Get-Content -Raw $clientPath
if ($clientRaw -notmatch "espTlsConnect\s*\([^;]*kAdsbCaBundle") {
  throw "$clientPath does not hand kAdsbCaBundle to espTlsConnect()."
}
Write-Host "OK: client passes kAdsbCaBundle into espTlsConnect()."

# --- 5. Trusted-time clock source: no raw time(nullptr) in the adapter -----
# The trusted-UTC adapter must derive "now" only from the accepted-sample
# monotonic clock (core::derivedTrustedNowUnix). The mutable system wall clock
# (::time) is set by lwIP via settimeofday BEFORE the sync callback runs -- even
# for a sample the core later rejects -- so it must never gate trusted state.
# Prove the adapter contains no raw time(nullptr)/time(NULL)/time(0) CALL. Comments
# and string literals are stripped first so documentation that merely names the
# pattern (e.g. a "no time(nullptr)" note) does not trip the scan; the match is
# then case-sensitive on lowercase `time(` so it never mistakes configTime(0,0,
# ...) (capital T) for a wall-clock read.
$timekeeperRaw = Get-Content -Raw $timekeeperPath
$timekeeperCode = [regex]::Replace($timekeeperRaw, "/\*.*?\*/", "",
  [System.Text.RegularExpressions.RegexOptions]::Singleline)  # block comments
$timekeeperCode = [regex]::Replace($timekeeperCode, "//[^\r\n]*", "")  # line comments
$timekeeperCode = [regex]::Replace($timekeeperCode, '"(?:[^"\\]|\\.)*"', '""')  # string bodies
if ($timekeeperCode -cmatch "\btime\s*\(\s*(?:nullptr|NULL|0)\s*\)") {
  throw ("$timekeeperPath reads the mutable system wall clock via " +
    "time(nullptr)/time(NULL): trusted-time decisions must use the derived " +
    "accepted-sample clock instead.")
}
Write-Host "OK: trusted-time adapter uses the derived accepted-sample clock (no raw time(nullptr))."

# --- 6. main passes the CA-authenticated peer-notBefore field to the floor ---
# main must hand the verified certificate provenance field
# (FetchResult.authenticated_cert_not_before_unix) into the persisted-floor
# service, and the superseded SNTP-time ratchet entry point (noteVerifiedFetch)
# must be gone -- proving the persisted floor is fed a CA-authenticated value, not
# an attacker-choosable SNTP timestamp.
$mainRaw = Get-Content -Raw $mainPath
# Strip comments so prose that merely names the old/new symbols cannot satisfy or
# trip the scan; the (?s) match spans the two-line call.
$mainCode = [regex]::Replace($mainRaw, "/\*.*?\*/", "",
  [System.Text.RegularExpressions.RegexOptions]::Singleline)
$mainCode = [regex]::Replace($mainCode, "//[^\r\n]*", "")
if ($mainCode -notmatch "(?s)noteVerifiedCertFloor\s*\([^;]*authenticated_cert_not_before_unix") {
  throw ("$mainPath does not pass the CA-authenticated peer-notBefore field " +
    "(authenticated_cert_not_before_unix) into noteVerifiedCertFloor().")
}
if ($mainCode -match "\bnoteVerifiedFetch\b") {
  throw ("$mainPath still calls the superseded SNTP-time ratchet " +
    "noteVerifiedFetch(); the persisted floor must be fed the certificate " +
    "notBefore via noteVerifiedCertFloor() instead.")
}
Write-Host "OK: main feeds authenticated_cert_not_before_unix to noteVerifiedCertFloor (no SNTP-time ratchet)."

# --- 7. Persisted-floor ratchet reads NO derived/SNTP clock for its NVS write --
# Isolate the noteVerifiedCertFloor function body (signature line through its
# column-0 closing brace; inner block braces are indented, so ^} matches only the
# function's own closing brace) and prove it contains no derived/SNTP clock read.
# The ratchet's candidate is the CA-signed certificate notBefore passed in by the
# caller, so persisting it must never consult nowUnix/derivedTrustedNowUnix or a
# trusted_now local. Comments and string bodies are stripped first so the doc
# comment (which names the clocks it avoids) cannot trip the scan.
$tkBodyMatch = [regex]::Match($timekeeperRaw,
  "(?ms)^void\s+noteVerifiedCertFloor\s*\(.*?^}")
if (-not $tkBodyMatch.Success) {
  throw "Could not locate the noteVerifiedCertFloor function body in $timekeeperPath."
}
$ratchetBody = $tkBodyMatch.Value
$ratchetCode = [regex]::Replace($ratchetBody, "/\*.*?\*/", "",
  [System.Text.RegularExpressions.RegexOptions]::Singleline)
$ratchetCode = [regex]::Replace($ratchetCode, "//[^\r\n]*", "")
$ratchetCode = [regex]::Replace($ratchetCode, '"(?:[^"\\]|\\.)*"', '""')
foreach ($banned in @("derivedTrustedNowUnix", "nowUnix", "trusted_now")) {
  if ($ratchetCode -match [regex]::Escape($banned)) {
    throw ("$timekeeperPath noteVerifiedCertFloor reads the derived/SNTP clock " +
      "('$banned'); the persisted-floor write must use ONLY the CA-signed " +
      "certificate notBefore passed in by the caller.")
  }
}
# And it must actually persist the caller-supplied authenticated candidate.
if ($ratchetCode -notmatch "shouldRatchetPersistedFloor\s*\([^;]*authenticated_cert_not_before_unix") {
  throw ("$timekeeperPath noteVerifiedCertFloor does not pass its " +
    "authenticated_cert_not_before_unix argument into shouldRatchetPersistedFloor().")
}
Write-Host ("OK: persisted-floor ratchet uses only the CA-signed certificate " +
  "notBefore (no nowUnix/derivedTrustedNowUnix/SNTP time for NVS).")

# --- 8. Full peer-chain date walk (no leaf-only / no removed chain walk) ------
# Because the pinned mbedTLS build omits CONFIG_MBEDTLS_HAVE_TIME_DATE for EVERY
# node, espVerifyPeerCertValidity must enforce notBefore/notAfter across the whole
# retained peer chain, not just the leaf. Prove, from the isolated function body,
# that it (a) traverses the chain via mbedtls_x509_crt::next and (b) feeds every
# node through the bounded Arduino-free accumulator (certChainBegin /
# certChainAddNode / certChainFinalize). Requiring BOTH catches a removed chain
# walk AND a leaf-only reversion (getPeerCertificate() + a single
# classifyCertVerification with no ->next traversal). Comments and string bodies
# are stripped first so the doc comment that names these symbols cannot satisfy
# the proof. The function body is the signature line through its column-0 closing
# brace (inner braces are indented, so ^} matches only the function's own end).
$verifyBodyMatch = [regex]::Match($transportRaw,
  "(?ms)^core::CertVerification\s+espVerifyPeerCertValidity\s*\(.*?^}")
if (-not $verifyBodyMatch.Success) {
  throw "Could not locate the espVerifyPeerCertValidity function body in $transportPath."
}
$verifyCode = [regex]::Replace($verifyBodyMatch.Value, "/\*.*?\*/", "",
  [System.Text.RegularExpressions.RegexOptions]::Singleline)
$verifyCode = [regex]::Replace($verifyCode, "//[^\r\n]*", "")
$verifyCode = [regex]::Replace($verifyCode, '"(?:[^"\\]|\\.)*"', '""')
if ($verifyCode -notmatch "->\s*next") {
  throw ("$transportPath espVerifyPeerCertValidity does not walk the peer chain " +
    "via mbedtls_x509_crt::next; a leaf-only date check is insufficient because " +
    "the pinned mbedTLS build enforces no node's notBefore/notAfter.")
}
foreach ($needed in @("certChainBegin", "certChainAddNode", "certChainFinalize")) {
  if ($verifyCode -notmatch [regex]::Escape($needed)) {
    throw ("$transportPath espVerifyPeerCertValidity does not drive the bounded " +
      "chain accumulator ('$needed' missing); the full-chain date walk must feed " +
      "each node through core::certChain{Begin,AddNode,Finalize}.")
  }
}
Write-Host ("OK: transport walks the full peer chain (mbedtls_x509_crt::next) " +
  "through the bounded certChain accumulator (no leaf-only date check).")

# And the core accumulator must actually BOUND the walk: certChainAddNode compares
# the running count against the fixed kMaxPeerChainLen cap before accepting a node,
# so a cyclic/over-long ::next list cannot spin. Isolate the function body and
# strip comments/strings so its doc comment cannot satisfy the proof.
$addNodeMatch = [regex]::Match((Get-Content -Raw $certTimePath),
  "(?ms)^bool\s+certChainAddNode\s*\(.*?^}")
if (-not $addNodeMatch.Success) {
  throw "Could not locate the certChainAddNode function body in $certTimePath."
}
$addNodeCode = [regex]::Replace($addNodeMatch.Value, "/\*.*?\*/", "",
  [System.Text.RegularExpressions.RegexOptions]::Singleline)
$addNodeCode = [regex]::Replace($addNodeCode, "//[^\r\n]*", "")
$addNodeCode = [regex]::Replace($addNodeCode, '"(?:[^"\\]|\\.)*"', '""')
if ($addNodeCode -notmatch "kMaxPeerChainLen") {
  throw ("$certTimePath certChainAddNode does not bound the chain against " +
    "kMaxPeerChainLen; the walk must fail closed past a fixed maximum.")
}
Write-Host "OK: core chain accumulator bounds the walk with a fixed kMaxPeerChainLen."

# --- Optional, NON-GATING live advisory ------------------------------------

if ($LiveAdvisory) {
  Write-Host "`n--- Live endpoint advisory (informational only; does NOT gate) ---"
  try {
    $client = [System.Net.Sockets.TcpClient]::new()
    $client.Connect("opendata.adsb.fi", 443)
    $ssl = [System.Net.Security.SslStream]::new(
      $client.GetStream(), $false,
      { param($s, $c, $ch, $e) $true })  # accept any: advisory only, no trust change
    $ssl.AuthenticateAsClient("opendata.adsb.fi")
    $leaf = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($ssl.RemoteCertificate)
    Write-Host ("Advisory: live leaf subject '{0}', issuer '{1}', valid {2}..{3}." -f `
        $leaf.Subject, $leaf.Issuer, $leaf.NotBefore.ToUniversalTime().ToString('yyyy-MM-dd'),
        $leaf.NotAfter.ToUniversalTime().ToString('yyyy-MM-dd'))
    $ssl.Dispose(); $client.Dispose()
  } catch {
    Write-Host "Advisory: live check skipped/failed (non-fatal): $($_.Exception.Message)"
  }
}

Write-Host "`nCA bundle gate verified: 4 pinned self-issued roots (SHA-256-pinned), no insecure TLS, CA enforced, derived-time clock, CA-authenticated cert-notBefore floor, bounded full peer-chain date walk."
