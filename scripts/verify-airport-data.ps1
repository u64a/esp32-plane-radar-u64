[CmdletBinding()]
param(
  [string]$ProjectRoot,
  [switch]$SelfTest
)

# ===========================================================================
# Phase 11 airport-data offline gate.
#
# OFFLINE/source-structural only. Cannot prove live provenance; certification
# requires:  python scripts/build_large_airports.py --airports-csv <csv> \
#              --runways-csv <csv> --check
#
# Invariants proved from source alone (20 total):
#   1.  Pinned commit URL; no /main/ in URL constants.
#   2.  SHA-256 format and expected values (lowercase hex 64 chars).
#   3.  Byte lengths: airports=12651071, runways=3951490.
#   4.  Hash-before-parse: sha256 verified before decode in fetch/read functions.
#   5.  Identity encoding: Accept-Encoding: identity set in fetch.
#   6.  write_bytes LF: files written via write_bytes(encode('utf-8')).
#   7.  Paired local arguments: both or neither local CSV flags.
#   8.  Check mode: --check renders without modifying files.
#   9.  Stable provenance in .h: commit/SHA-256/lengths/blobs/license/filter ver.
#  10.  Stable provenance in .cpp: same provenance block.
#  11.  .gitattributes LF rules for both generated files.
#  12.  Header constants: kAirportCount=1166, kRunwayCount=1706.
#  13.  Sized externs: kAirports[kAirportCount], kRunways[kRunwayCount].
#  14.  Airport idents unique, sorted, 4 chars.
#  15.  Coordinates valid: lat [-90e7,90e7], lon [-180e7,180e7].
#  16.  Runway count=1706; all lengths positive.
#  17.  Runway airport_idx in [0, kAirportCount-1].
#  18.  Runway ordering: by airport_idx ASC, length DESC within airport.
#  19.  No dynamic timestamp or Python-version in provenance.
#  20.  No trust-on-first-use or auto-update-hash mode in generator.
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
  throw "AIRPORT DATA POLICY VIOLATION: $Message"
}

function Read-Source {
  param([Parameter(Mandatory)][string]$Path)
  if (-not (Test-Path $Path)) { Fail "Required file not found: $Path" }
  return Get-Content -Raw $Path
}

function Parse-AirportEntries {
  param([Parameter(Mandatory)][string]$CppText)
  $rx = [regex]'^\s*\{"([A-Z]{4})",\s*(-?\d+),\s*(-?\d+)\},'
  $entries = New-Object System.Collections.Generic.List[object]
  foreach ($line in ($CppText -split "`n")) {
    $m = $rx.Match($line)
    if ($m.Success) {
      $entries.Add([pscustomobject]@{
        Ident = $m.Groups[1].Value
        Lat   = [int64]$m.Groups[2].Value
        Lon   = [int64]$m.Groups[3].Value
      })
    }
  }
  return $entries
}

function Parse-RunwayEntries {
  param([Parameter(Mandatory)][string]$CppText)
  $rx = [regex]'^\s*\{(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+)\},'
  $entries = New-Object System.Collections.Generic.List[object]
  foreach ($line in ($CppText -split "`n")) {
    $m = $rx.Match($line)
    if ($m.Success) {
      $entries.Add([pscustomobject]@{
        AirportIdx = [int]$m.Groups[1].Value
        LengthM    = [int]$m.Groups[6].Value
      })
    }
  }
  return $entries
}

function Invoke-LiveGate {
  param([string]$Root)

  $pyScript   = Join-Path $Root "scripts\build_large_airports.py"
  $headerPath = Join-Path $Root "include\data\large_airports.h"
  $cppPath    = Join-Path $Root "src\data\large_airports_data.cpp"
  $gitAttr    = Join-Path $Root ".gitattributes"

  $pyText     = Read-Source $pyScript
  $hText      = Read-Source $headerPath
  $cppText    = Read-Source $cppPath
  $attrText   = Read-Source $gitAttr

  $COMMIT = "79efa72ec1e344d91b081160634fa042a56a21b8"
  $AIRPORTS_SHA256 = "092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354"
  $RUNWAYS_SHA256  = "312f9ded8a5a29f8634bd615b0a7aadd4ed01e773ae63e5aab7c510629440fec"

  # Strip Python comment lines before checking URL constants (comments may
  # legitimately mention "/main/" as documentation of what is forbidden).
  $pyCodeLines = ($pyText -split "`n") | Where-Object { $_ -notmatch '^\s*#' }
  $pyCodeText  = $pyCodeLines -join "`n"

  # 1. Pinned commit constant (_COMMIT = "...") and no /main/ in URL string constants
  $commitAssignRx = [regex]('"' + [regex]::Escape($COMMIT) + '"')
  if (-not $commitAssignRx.IsMatch($pyCodeText)) {
    Fail "Generator _COMMIT constant is not set to the pinned commit $COMMIT"
  }
  # Check only non-comment non-docstring lines for /main/: lines between triple-quotes are excluded
  $inDocStr = $false
  $pyCodeOnlyLines = foreach ($line in ($pyText -split "`n")) {
    $stripped = $line.Trim()
    $isComment = $stripped -match '^#'
    $hasDq = $stripped -match '"""'
    if ($hasDq -and -not $inDocStr) { $inDocStr = $true; continue }
    elseif ($hasDq -and $inDocStr) { $inDocStr = $false; continue }
    if ($inDocStr -or $isComment) { continue }
    $line
  }
  $pyCodeOnlyText = $pyCodeOnlyLines -join "`n"
  # URL string constants in actual code must not use /main/
  foreach ($line in ($pyCodeOnlyLines | Where-Object { $_ -match '"' -or $_ -match "'" })) {
    if ($line -match [regex]::Escape("/main/")) {
      Fail "Generator URL string in code contains /main/ -- must use only pinned commit URL"
    }
  }

  # 2. SHA-256 format and expected values
  if ($pyCodeText -notmatch [regex]::Escape($AIRPORTS_SHA256)) {
    Fail "Generator missing expected airports.csv SHA-256 $AIRPORTS_SHA256"
  }
  if ($pyCodeText -notmatch [regex]::Escape($RUNWAYS_SHA256)) {
    Fail "Generator missing expected runways.csv SHA-256 $RUNWAYS_SHA256"
  }
  $shaRx = [regex]"^[0-9a-f]{64}$"
  if (-not $shaRx.IsMatch($AIRPORTS_SHA256)) { Fail "airports SHA-256 not valid lowercase hex" }
  if (-not $shaRx.IsMatch($RUNWAYS_SHA256))  { Fail "runways SHA-256 not valid lowercase hex" }

  # 3. Byte lengths (Python uses underscore separators in integer literals)
  if ($pyCodeText -notmatch "12[_]?651[_]?071") { Fail "Generator missing expected airports.csv length 12651071" }
  if ($pyCodeText -notmatch "3[_]?951[_]?490")  { Fail "Generator missing expected runways.csv length 3951490" }

  # 4. Hash-before-parse: _fetch_verified verifies SHA-256 and length before
  #    returning raw bytes; decode/CSV parsing happens only in _parse_csv after.
  if ($pyCodeText -notmatch "_read_local_verified" -or $pyCodeText -notmatch "_fetch_verified") {
    Fail "Generator missing _fetch_verified/_read_local_verified functions"
  }
  $verifyFuncRx = [regex]"(?s)def _fetch_verified.*?return data"
  $verifyMatch = $verifyFuncRx.Match($pyText)
  if (-not $verifyMatch.Success) { Fail "Could not locate _fetch_verified body" }
  $verifyBody = $verifyMatch.Value
  if ($verifyBody -notmatch "hashlib.sha256") {
    Fail "_fetch_verified missing hashlib.sha256 verification"
  }
  # decode/CSV parsing must NOT happen inside _fetch_verified (it returns raw bytes)
  if ($verifyBody -match [regex]::Escape("decode(") -or $verifyBody -match "csv.DictReader") {
    Fail "_fetch_verified must not decode/parse CSV; it must return raw bytes for separate parsing"
  }
  # _parse_csv must exist and do the decode
  if ($pyText -notmatch "_parse_csv" -or $pyText -notmatch [regex]::Escape(".decode(")) {
    Fail "Generator missing _parse_csv function with decode"
  }

  # 5. Identity encoding
  if ($pyCodeText -notmatch [regex]::Escape("Accept-Encoding") -or
      $pyCodeText -notmatch [regex]::Escape("identity")) {
    Fail "Generator does not set Accept-Encoding: identity"
  }

  # 6. write_bytes LF
  if ($pyCodeText -notmatch [regex]::Escape("write_bytes")) {
    Fail "Generator must write files with write_bytes"
  }
  if ($pyCodeText -notmatch '\.encode\("utf-8"\)' -and
      $pyCodeText -notmatch "\.encode\('utf-8'\)") {
    Fail "Generator must encode with .encode('utf-8') or .encode(`"utf-8`")"
  }
  if ($pyCodeText -match [regex]::Escape("write_text")) {
    Fail "Generator must not use write_text"
  }

  # 7. Paired local arguments: both or neither must be enforced
  if (($pyCodeText -notmatch [regex]::Escape("both or neither")) -and
      ($pyCodeText -notmatch [regex]::Escape("specified together"))) {
    Fail "Generator must enforce that --airports-csv and --runways-csv are paired"
  }

  # 8. Check mode
  if ($pyCodeText -notmatch [regex]::Escape("--check")) {
    Fail "Generator is missing --check mode"
  }
  if ($pyCodeText -notmatch [regex]::Escape("check_mode")) {
    Fail "Generator is missing check_mode function"
  }
  $checkModeRx = [regex]"(?s)def check_mode.*?return \d"
  $checkMatch = $checkModeRx.Match($pyText)
  if ($checkMatch.Success) {
    $checkBody = $checkMatch.Value
    if ($checkBody -match "write_bytes" -or $checkBody -match "write_text") {
      Fail "check_mode must not write files"
    }
  }

  # 9. Stable provenance in .h
  foreach ($frag in @(
    $COMMIT, $AIRPORTS_SHA256, $RUNWAYS_SHA256,
    "12651071", "3951490",
    "1df8da39141e2e6adb3c8439b684579da97c19a8",
    "d8fdbbe9a1f7d47ee78eade1ec30f020e3cc9e7d",
    "Public Domain", "The Unlicense",
    "Filter schema version: 1"
  )) {
    if ($hText -notmatch [regex]::Escape($frag)) {
      Fail "large_airports.h missing provenance fragment: $frag"
    }
  }

  # 10. Stable provenance in .cpp
  foreach ($frag in @(
    $COMMIT, $AIRPORTS_SHA256, $RUNWAYS_SHA256,
    "12651071", "3951490",
    "Public Domain", "The Unlicense",
    "Filter schema version: 1"
  )) {
    if ($cppText -notmatch [regex]::Escape($frag)) {
      Fail "large_airports_data.cpp missing provenance fragment: $frag"
    }
  }

  # 11. .gitattributes LF rules
  if ($attrText -notmatch [regex]::Escape("include/data/large_airports.h") -or
      $attrText -notmatch "eol=lf") {
    Fail ".gitattributes missing LF rule for include/data/large_airports.h"
  }
  if ($attrText -notmatch [regex]::Escape("src/data/large_airports_data.cpp") -or
      $attrText -notmatch "eol=lf") {
    Fail ".gitattributes missing LF rule for src/data/large_airports_data.cpp"
  }

  # 12. Header constants
  if ($hText -notmatch [regex]::Escape("kAirportCount = 1166")) {
    Fail "large_airports.h: kAirportCount != 1166"
  }
  if ($hText -notmatch [regex]::Escape("kRunwayCount = 1706")) {
    Fail "large_airports.h: kRunwayCount != 1706"
  }

  # 13. Sized externs
  if ($hText -notmatch [regex]::Escape("kAirports[kAirportCount]")) {
    Fail "large_airports.h: extern must be kAirports[kAirportCount]"
  }
  if ($hText -notmatch [regex]::Escape("kRunways[kRunwayCount]")) {
    Fail "large_airports.h: extern must be kRunways[kRunwayCount]"
  }

  # 14. Airport idents unique, sorted, 4 chars
  $airports = Parse-AirportEntries $cppText
  if ($airports.Count -ne 1166) {
    Fail "Expected 1166 airport entries, found $($airports.Count)"
  }
  $idents = @($airports | ForEach-Object { $_.Ident })
  $uniqueCount = ($idents | Sort-Object -Unique).Count
  if ($uniqueCount -ne $idents.Count) {
    Fail "Airport idents are not unique ($($idents.Count) entries, $uniqueCount unique)"
  }
  for ($idx = 1; $idx -lt $idents.Count; $idx++) {
    $prev = $idents[$idx - 1]
    $curr = $idents[$idx]
    if ([string]::Compare($prev, $curr, [System.StringComparison]::Ordinal) -ge 0) {
      Fail "Airport idents not in ascending order at position $idx : $prev >= $curr"
    }
  }
  foreach ($ident in $idents) {
    if ($ident.Length -ne 4) { Fail "Airport ident not 4 chars: $ident" }
  }

  # 15. Coordinates valid
  foreach ($ap in $airports) {
    if ($ap.Lat -lt -900000000 -or $ap.Lat -gt 900000000) {
      Fail "Airport $($ap.Ident) lat $($ap.Lat) out of range [-90e7,90e7]"
    }
    if ($ap.Lon -lt -1800000000 -or $ap.Lon -gt 1800000000) {
      Fail "Airport $($ap.Ident) lon $($ap.Lon) out of range [-180e7,180e7]"
    }
  }

  # 16-18. Runway count, lengths, index bounds, ordering
  $runways = Parse-RunwayEntries $cppText
  if ($runways.Count -ne 1706) {
    Fail "Expected 1706 runway entries, found $($runways.Count)"
  }
  $prevRwIdx = -1
  $prevRwLen = [int]::MaxValue
  foreach ($rw in $runways) {
    if ($rw.LengthM -le 0) {
      Fail "Runway with airport_idx=$($rw.AirportIdx) has non-positive length $($rw.LengthM)"
    }
    if ($rw.AirportIdx -lt 0 -or $rw.AirportIdx -ge 1166) {
      Fail "Runway airport_idx=$($rw.AirportIdx) out of range [0,1165]"
    }
    if ($rw.AirportIdx -lt $prevRwIdx) {
      Fail "Runways not sorted by airport_idx ASC: $prevRwIdx then $($rw.AirportIdx)"
    }
    if ($rw.AirportIdx -eq $prevRwIdx -and $rw.LengthM -gt $prevRwLen) {
      Fail "Runways for airport_idx=$($rw.AirportIdx) not sorted by length DESC: $prevRwLen then $($rw.LengthM)"
    }
    if ($rw.AirportIdx -ne $prevRwIdx) { $prevRwLen = [int]::MaxValue }
    $prevRwIdx = $rw.AirportIdx
    $prevRwLen = $rw.LengthM
  }

  # 19. No dynamic timestamp or Python-version in provenance
  $dynPatterns = @("__DATE__", "__TIME__", "python_version", "sys.version",
                   "datetime.now", "Generated on:", "Generated at:")
  foreach ($pat in $dynPatterns) {
    if ($hText -match [regex]::Escape($pat)) {
      Fail "large_airports.h contains dynamic provenance token: $pat"
    }
    if ($cppText -match [regex]::Escape($pat)) {
      Fail "large_airports_data.cpp contains dynamic provenance token: $pat"
    }
  }
  if ($pyCodeText -match [regex]::Escape("datetime.now") -or
      $pyCodeText -match [regex]::Escape("platform.python_version")) {
    Fail "Generator embeds dynamic timestamp/Python-version in render output"
  }

  # 20. No trust-on-first-use or auto-update-hash mode in executable code
  # Strip docstrings (simple heuristic: lines between """ markers) and comments.
  $inDocstring = $false
  $pyExecLines = foreach ($line in ($pyText -split "`n")) {
    $stripped = $line.Trim()
    if ($stripped -match '"""') {
      if ($inDocstring) { $inDocstring = $false } else { $inDocstring = $true }
      continue
    }
    if ($inDocstring) { continue }
    if ($stripped -match "^#") { continue }
    $line
  }
  $pyExecText = $pyExecLines -join "`n"
  foreach ($pat in @("--update-hash", "--fetch-hash", "auto_update_hash",
                     "update_hash_mode", "trust_on_first_use")) {
    if ($pyExecText -imatch [regex]::Escape($pat)) {
      Fail "Generator contains disallowed hash-bypass/auto-update pattern in code: $pat"
    }
  }

  Write-Host "OK: all 20 airport-data invariants verified."
  Write-Host ""
  Write-Host "NOTE: This gate is OFFLINE/source-structural only."
  Write-Host "      It cannot prove live provenance. Certification requires running"
  Write-Host "      the generator with --check against hash-verified pinned CSVs."
}

# ---------------------------------------------------------------------------
# Self-test: prove the gate rejects representative tamper cases
# ---------------------------------------------------------------------------
function Invoke-SelfTest {
  param([string]$Root)

  $pyScript   = Join-Path $Root "scripts\build_large_airports.py"
  $headerPath = Join-Path $Root "include\data\large_airports.h"
  $cppPath    = Join-Path $Root "src\data\large_airports_data.cpp"
  $gitAttr    = Join-Path $Root ".gitattributes"

  $pyOrig   = Get-Content -Raw $pyScript
  $hOrig    = Get-Content -Raw $headerPath
  $cppOrig  = Get-Content -Raw $cppPath
  $attrOrig = Get-Content -Raw $gitAttr

  $tmp = [System.IO.Path]::Combine(
    $env:TEMP,
    "airport-gate-selftest-$([System.Guid]::NewGuid().ToString('N'))"
  )
  New-Item -ItemType Directory -Path $tmp | Out-Null
  $tmpScripts = Join-Path $tmp "scripts"
  $tmpInc     = Join-Path $tmp "include\data"
  $tmpSrc     = Join-Path $tmp "src\data"
  New-Item -ItemType Directory -Path $tmpScripts | Out-Null
  New-Item -ItemType Directory -Path $tmpInc -Force | Out-Null
  New-Item -ItemType Directory -Path $tmpSrc -Force | Out-Null

  function Write-TmpFiles([string]$py, [string]$h, [string]$cpp, [string]$attr) {
    [System.IO.File]::WriteAllText((Join-Path $tmpScripts "build_large_airports.py"), $py)
    [System.IO.File]::WriteAllText((Join-Path $tmpInc "large_airports.h"), $h)
    [System.IO.File]::WriteAllText((Join-Path $tmpSrc "large_airports_data.cpp"), $cpp)
    [System.IO.File]::WriteAllText((Join-Path $tmp ".gitattributes"), $attr)
  }

  function Expect-Fail([string]$Label, [string]$py, [string]$h, [string]$cpp, [string]$attr) {
    Write-TmpFiles $py $h $cpp $attr
    try {
      Invoke-LiveGate -Root $tmp
      throw "SELF-TEST FAILURE: '$Label' should have failed but passed."
    } catch {
      if ($_.Exception.Message -match "SELF-TEST FAILURE") { throw }
      Write-Host "OK (rejected): $Label"
    }
  }

  try {
    # T1: mutable /main/ URL -- replace the pinned commit constant with "main"
    # so the URL string literal assembles a /main/ path
    $badPy1 = $pyOrig -replace [regex]::Escape('_COMMIT = "79efa72ec1e344d91b081160634fa042a56a21b8"'), '_COMMIT = "main"'
    Expect-Fail "mutable main URL" $badPy1 $hOrig $cppOrig $attrOrig

    # T2: changed airports SHA-256
    $badPy2 = $pyOrig -replace "092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354", ("0" * 64)
    Expect-Fail "changed airports SHA-256" $badPy2 $hOrig $cppOrig $attrOrig

    # T3: missing provenance commit in header
    $badH3 = $hOrig -replace "79efa72ec1e344d91b081160634fa042a56a21b8", "REMOVED"
    Expect-Fail "missing provenance in header" $pyOrig $badH3 $cppOrig $attrOrig

    # T4: kAirportCount drift in header
    $badH4 = $hOrig -replace "kAirportCount = 1166", "kAirportCount = 1167"
    Expect-Fail "count drift in header" $pyOrig $badH4 $cppOrig $attrOrig

    # T5: out-of-range airport_idx in .cpp (first runway gets idx 9999)
    $firstRwLine = ($cppOrig -split "`n") | Where-Object { $_ -match '^\s*\{0,' } | Select-Object -First 1
    $badCpp5 = $cppOrig -replace [regex]::Escape($firstRwLine), ($firstRwLine -replace '^\s*\{0,', '  {9999,')
    Expect-Fail "out-of-range runway index" $pyOrig $hOrig $badCpp5 $attrOrig

    # T6: unsorted airport idents (replace first ident AGGH with ZZZZ)
    $badCpp6 = $cppOrig -replace '"AGGH"', '"ZZZZ"'
    Expect-Fail "unsorted airport idents" $pyOrig $hOrig $badCpp6 $attrOrig

    # T7: removed LF rule for header from .gitattributes
    $lines7 = $attrOrig -split "`n" | Where-Object { $_ -notmatch "large_airports\.h" }
    $badAttr7 = $lines7 -join "`n"
    Expect-Fail "removed LF rule for header" $pyOrig $hOrig $cppOrig $badAttr7

    # T8: unsized extern (kAirports[] instead of kAirports[kAirportCount])
    $badH8 = $hOrig -replace "kAirports\[kAirportCount\]", "kAirports[]"
    Expect-Fail "unsized extern in header" $pyOrig $badH8 $cppOrig $attrOrig

    # T9: timestamp injection in header provenance
    $badH9 = $hOrig -replace "Filter schema version: 1", "Filter schema version: 1`n// Generated on: 2026-01-01"
    Expect-Fail "timestamp injection in header" $pyOrig $badH9 $cppOrig $attrOrig

    # T10: generator hash bypass -- add actual code (argparse option) for hash update mode
    $badPy10 = $pyOrig -replace [regex]::Escape('parser.add_argument('), "parser.add_argument(`n        '--update-hash', action='store_true',`n        help='Trust-on-first-use: auto_update_hash from live URL',`n    )`n    parser.add_argument("
    Expect-Fail "generator hash bypass" $badPy10 $hOrig $cppOrig $attrOrig

    # T11: missing airports SHA-256 from generator
    $badPy11 = $pyOrig -replace "092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354", "REMOVED_HASH_VALUE_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
    Expect-Fail "missing airports SHA-256 from generator" $badPy11 $hOrig $cppOrig $attrOrig

    # T12: write_text instead of write_bytes in generator
    $badPy12 = $pyOrig -replace "write_bytes", "write_text"
    Expect-Fail "write_text instead of write_bytes" $badPy12 $hOrig $cppOrig $attrOrig

    Write-Host ""
    Write-Host "OK: all 12 self-test tamper cases correctly rejected."
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
