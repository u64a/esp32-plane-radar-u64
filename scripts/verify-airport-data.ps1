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
#   1.  Exact pinned _COMMIT/_BASE_URL/AIRPORTS_URL/RUNWAYS_URL/LICENSE_URL
#       assignments; pinned commit URL composition; no /main/ in URL constants.
#   2.  SHA-256 captured from the generator's own assignments, validated as
#       lowercase hex 64 chars, and required to equal the pinned values.
#   3.  Exact AIRPORTS_LENGTH/RUNWAYS_LENGTH assignments: airports=12651071,
#       runways=3951490.
#   4.  Hash-before-parse: each verifier compares length to expected_length AND
#       computed SHA-256 to expected_sha256 before returning raw bytes.
#   5.  Identity encoding: Accept-Encoding: identity set in fetch.
#   6.  Exact-LF check/write: check mode compares read_bytes directly to rendered
#       UTF-8 LF bytes; files are written via write_bytes(encode('utf-8')).
#   7.  Paired local arguments: both or neither local CSV flags.
#   8.  Check mode: --check renders without modifying files; the
#       'def check_mode ... return' body MUST be locatable (FAIL otherwise).
#   9.  Stable provenance in .h: commit/SHA-256/lengths/blobs/license/filter ver.
#  10.  Stable provenance in .cpp: same provenance block.
#  11.  .gitattributes LF rules for both generated files.
#  12.  Header constants: kAirportCount=1166, kRunwayCount=1706.
#  13.  Sized externs: kAirports[kAirportCount], kRunways[kRunwayCount].
#  14.  Airport idents unique, sorted, 4 chars.
#  15.  Airport and runway endpoint coordinates valid: lat [-90e7,90e7],
#       lon [-180e7,180e7].
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

# Python lexical skeleton helper for the generator's ordinary syntax. It preserves
# newlines and blanks comments and triple-quoted strings. With -BlankStrings it
# also blanks ordinary string contents, so executable checks cannot be faked by
# docstrings or string literals.
function Get-PythonSkeleton {
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Text, [switch]$BlankStrings)
  $out = New-Object System.Text.StringBuilder
  $i = 0
  while ($i -lt $Text.Length) {
    $ch = $Text[$i]
    if ($ch -eq '#') {
      while ($i -lt $Text.Length -and $Text[$i] -ne "`n") { [void]$out.Append(' '); $i++ }
      continue
    }
    if ($ch -eq '"' -or $ch -eq "'") {
      $quote = [string]$ch
      $triple = ($i + 2 -lt $Text.Length -and $Text.Substring($i, 3) -eq ($quote * 3))
      if ($triple) {
        for ($j = 0; $j -lt 3; $j++) { [void]$out.Append(' ') }; $i += 3
        while ($i -lt $Text.Length) {
          if ($i + 2 -lt $Text.Length -and $Text.Substring($i, 3) -eq ($quote * 3)) {
            for ($j = 0; $j -lt 3; $j++) { [void]$out.Append(' ') }; $i += 3; break
          }
          if ($Text[$i] -eq "`n") { [void]$out.Append("`n") } else { [void]$out.Append(' ') }
          $i++
        }
        continue
      }
      [void]$out.Append($ch); $i++
      while ($i -lt $Text.Length) {
        $current = $Text[$i]
        if ($current -eq '\' -and $i + 1 -lt $Text.Length) {
          if ($BlankStrings) { [void]$out.Append(' '); [void]$out.Append(' ') }
          else { [void]$out.Append($current); [void]$out.Append($Text[$i + 1]) }
          $i += 2; continue
        }
        if ($current -eq $ch) { [void]$out.Append($current); $i++; break }
        if ($BlankStrings) {
          if ($current -eq "`n") { [void]$out.Append("`n") } else { [void]$out.Append(' ') }
        } else { [void]$out.Append($current) }
        $i++
      }
      continue
    }
    [void]$out.Append($ch); $i++
  }
  return $out.ToString()
}

function Get-PythonTopLevelFunctionBody {
  param([Parameter(Mandatory)][string]$Skeleton, [Parameter(Mandatory)][string]$Name)
  $start = [regex]::Match($Skeleton, '(?m)^def\s+' + [regex]::Escape($Name) + '\b[^\r\n]*:\s*$')
  if (-not $start.Success) { return $null }
  $next = ([regex]'(?m)^def\s+').Match($Skeleton, $start.Index + $start.Length)
  $end = if ($next.Success) { $next.Index } else { $Skeleton.Length }
  return $Skeleton.Substring($start.Index, $end - $start.Index)
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
        LeLat      = [int64]$m.Groups[2].Value
        LeLon      = [int64]$m.Groups[3].Value
        HeLat      = [int64]$m.Groups[4].Value
        HeLon      = [int64]$m.Groups[5].Value
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

  # Literal-preserved skeleton supports exact constants; executable checks use
  # a strings-blanked skeleton so docstrings/literals cannot satisfy them.
  $pyCodeText = Get-PythonSkeleton $pyText
  $pyExecText = Get-PythonSkeleton $pyText -BlankStrings

  # 1. Exact pinned constants and pinned URL composition. A changed live URL must
  #    fail even if the old pinned commit string still appears elsewhere.
  if ($pyCodeText -notmatch ('(?m)^\s*_COMMIT\s*=\s*"' + [regex]::Escape($COMMIT) + '"\s*$')) {
    Fail "Generator _COMMIT must be exactly _COMMIT = `"$COMMIT`""
  }
  $baseUrlRx = [regex]('(?ms)^\s*_BASE_URL\s*=\s*\(\s*"https://raw\.githubusercontent\.com/davidmegginson/ourairports-data/"\s*\+\s*_COMMIT\s*\)\s*$')
  if (-not $baseUrlRx.IsMatch($pyCodeText)) {
    Fail "Generator _BASE_URL must be composed from the pinned raw.githubusercontent base + _COMMIT"
  }
  foreach ($u in @(
    @{ Name = "AIRPORTS_URL"; Suffix = "/airports.csv" },
    @{ Name = "RUNWAYS_URL";  Suffix = "/runways.csv" },
    @{ Name = "LICENSE_URL";  Suffix = "/LICENSE" }
  )) {
    $urlAssignRx = [regex]('(?m)^\s*' + $u.Name + '\s*=\s*_BASE_URL\s*\+\s*"' + [regex]::Escape($u.Suffix) + '"\s*$')
    if (-not $urlAssignRx.IsMatch($pyCodeText)) {
      Fail ("Generator " + $u.Name + " must be exactly _BASE_URL + `"" + $u.Suffix + "`"")
    }
  }
  # The literal-preserved skeleton excludes comments and triple-quoted docstrings.
  if ($pyCodeText -match [regex]::Escape("/main/")) {
    Fail "Generator code contains /main/ -- must use only pinned commit URL"
  }

  # 2. SHA-256: capture the ACTUAL values assigned in the generator, validate the
  #    lowercase-64-hex format of the CAPTURED value (not just the hardcoded
  #    PowerShell expectation), and require the pinned value.
  foreach ($s in @(
    @{ Name = "AIRPORTS_SHA256"; Expected = $AIRPORTS_SHA256 },
    @{ Name = "RUNWAYS_SHA256";  Expected = $RUNWAYS_SHA256 }
  )) {
    $shaAssignRx = [regex]('(?m)^\s*' + $s.Name + '\s*=\s*"([^"]*)"\s*$')
    $shaMatch = $shaAssignRx.Match($pyCodeText)
    if (-not $shaMatch.Success) {
      Fail ("Generator " + $s.Name + " string assignment not found")
    }
    $captured = $shaMatch.Groups[1].Value
    if ($captured -cnotmatch '^[0-9a-f]{64}$') {
      Fail ("Generator " + $s.Name + " value '" + $captured + "' is not lowercase 64-hex")
    }
    if ($captured -cne $s.Expected) {
      Fail ("Generator " + $s.Name + " ('" + $captured + "') != pinned " + $s.Expected)
    }
  }

  # 3. Byte lengths: exact constant assignments (underscore separators optional).
  if ($pyCodeText -notmatch '(?m)^\s*AIRPORTS_LENGTH\s*=\s*12_?651_?071\s*$') {
    Fail "Generator AIRPORTS_LENGTH must be exactly 12_651_071"
  }
  if ($pyCodeText -notmatch '(?m)^\s*RUNWAYS_LENGTH\s*=\s*3_?951_?490\s*$') {
    Fail "Generator RUNWAYS_LENGTH must be exactly 3_951_490"
  }

  # 4. Bounded verifier bodies must execute exact raw-byte checks before return.
  foreach ($functionName in @("_fetch_verified", "_read_local_verified")) {
    $verifyBody = Get-PythonTopLevelFunctionBody $pyExecText $functionName
    if ($null -eq $verifyBody) { Fail "Could not locate $functionName body" }
    $returnMatch = [regex]::Match($verifyBody, '(?m)^\s*return\s+data\s*$')
    if (-not $returnMatch.Success) { Fail "$functionName must return data in its own body" }
    foreach ($required in @(
      @{ Pat = '(?m)^\s*actual_len\s*=\s*len\s*\(\s*data\s*\)\s*$'; Msg = 'actual_len = len(data)' },
      @{ Pat = '(?m)^\s*if\s+actual_len\s*!=\s*expected_length\s*:\s*$'; Msg = 'if actual_len != expected_length:' },
      @{ Pat = '(?m)^\s*actual_sha\s*=\s*hashlib\.sha256\s*\(\s*data\s*\)\.hexdigest\s*\(\s*\)\s*$'; Msg = 'actual_sha = hashlib.sha256(data).hexdigest()' },
      @{ Pat = '(?m)^\s*if\s+actual_sha\s*!=\s*expected_sha256\s*:\s*$'; Msg = 'if actual_sha != expected_sha256:' }
    )) {
      $match = [regex]::Match($verifyBody, $required.Pat)
      if (-not $match.Success -or $match.Index -gt $returnMatch.Index) { Fail "$functionName must execute '$($required.Msg)' before return data" }
    }
    if ($verifyBody -match '\bdecode\s*\(' -or $verifyBody -match 'csv\.DictReader') { Fail "$functionName must not decode/parse CSV" }
  }
  $fetchLiteralBody = Get-PythonTopLevelFunctionBody $pyCodeText "_fetch_verified"
  if ($null -eq $fetchLiteralBody -or $fetchLiteralBody -notmatch '(?ms)^\s*req\s*=\s*urllib\.request\.Request\s*\(\s*url\s*,\s*headers\s*=\s*\{\s*"Accept-Encoding"\s*:\s*"identity"\s*\}\s*\)\s*$') {
    Fail '_fetch_verified must construct Request(url, headers={"Accept-Encoding": "identity"})'
  }
  if ($pyExecText -notmatch '(?m)^def\s+_parse_csv\b' -or $pyExecText -notmatch '\.decode\s*\(') { Fail "Generator missing _parse_csv function with decode" }

  # 5. Exact deterministic writes must be executable statements in main. The
  # literal-preserved body excludes comments and triple-quoted docstrings; anchors
  # reject ordinary string literals that merely contain a write-looking call.
  $mainLiteralBody = Get-PythonTopLevelFunctionBody $pyCodeText "main"
  if ($null -eq $mainLiteralBody) { Fail "Could not locate main body for output-write checks" }
  foreach ($write in @(
    'OUT_H\.write_bytes\(header\.encode\("utf-8"\)\)',
    'OUT_CPP\.write_bytes\(cpp\.encode\("utf-8"\)\)'
  )) {
    if ($mainLiteralBody -notmatch ('(?m)^\s*' + $write + '\s*$')) {
      Fail "main must use deterministic UTF-8 write_bytes output"
    }
  }
  if ($pyExecText -match '\bwrite_text\s*\(') { Fail "Generator must not use write_text" }

  # 6. Paired local arguments must be executable main-body enforcement.
  $mainExecBody = Get-PythonTopLevelFunctionBody $pyExecText "main"
  if ($null -eq $mainExecBody -or $mainExecBody -notmatch '(?ms)^\s*if\s+bool\s*\(\s*local_airports\s*\)\s*!=\s*bool\s*\(\s*local_runways\s*\)\s*:\s*\r?\n\s*parser\.error\s*\(') { Fail "main must enforce paired local CSV arguments with parser.error" }

  # 7. check_mode is independently bounded and side-effect free.
  if ($pyCodeText -notmatch [regex]::Escape("--check")) { Fail "Generator is missing --check mode" }
  $checkBody = Get-PythonTopLevelFunctionBody $pyExecText "check_mode"
  if ($null -eq $checkBody) { Fail "Could not locate check_mode body" }
  if ($checkBody -match '\bwrite_bytes\s*\(' -or $checkBody -match '\bwrite_text\s*\(') { Fail "check_mode must not write files" }
  if ($checkBody -notmatch '(?m)^\s*on_disk\s*=\s*path\.read_bytes\s*\(\s*\)\s*$') { Fail "check_mode must compare path.read_bytes() directly to rendered bytes" }
  if ($checkBody -match 'path\.read_bytes\s*\(\s*\)\s*\.' -or $checkBody -match '\.replace\s*\(') { Fail "check_mode must not transform on-disk bytes" }
  if ($checkBody -notmatch '(?m)^\s*return\s+0\s+if\s+ok\s+else\s+1\s*$') { Fail "check_mode must return 0 if ok else 1 in its own body" }

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
  foreach ($generatedPath in @("include/data/large_airports.h",
                                "src/data/large_airports_data.cpp")) {
    $lineRx = [regex]("(?m)^" + [regex]::Escape($generatedPath) + "\s+.*(?:^|\s)eol=lf(?:\s|$)")
    if (-not $lineRx.IsMatch($attrText)) {
      Fail ".gitattributes missing same-line exact LF rule for $generatedPath"
    }
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

  # 15. Airport and runway endpoint coordinates valid.
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
    foreach ($endpoint in @(
      @{ Name = "le_lat_e7"; Value = $rw.LeLat; Min = -900000000; Max = 900000000 },
      @{ Name = "he_lat_e7"; Value = $rw.HeLat; Min = -900000000; Max = 900000000 },
      @{ Name = "le_lon_e7"; Value = $rw.LeLon; Min = -1800000000; Max = 1800000000 },
      @{ Name = "he_lon_e7"; Value = $rw.HeLon; Min = -1800000000; Max = 1800000000 }
    )) {
      if ($endpoint.Value -lt $endpoint.Min -or $endpoint.Value -gt $endpoint.Max) {
        Fail "Runway airport_idx=$($rw.AirportIdx) $($endpoint.Name)=$($endpoint.Value) out of range [$($endpoint.Min),$($endpoint.Max)]"
      }
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

  # 20. No trust-on-first-use or auto-update-hash mode in executable code.
  foreach ($pat in @("--update-hash", "--fetch-hash", "auto_update_hash", "update_hash_mode", "trust_on_first_use")) {
    if ($pyCodeText -imatch [regex]::Escape($pat)) { Fail "Generator contains disallowed hash-bypass/auto-update pattern in code: $pat" }
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
      if (-not $_.Exception.Message.StartsWith("AIRPORT DATA POLICY VIOLATION:")) { throw }
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

    # T7: an LF rule on another path must not satisfy the header's own rule.
    $badAttr7 = $attrOrig -replace [regex]::Escape("include/data/large_airports.h       text eol=lf"),
      "include/data/large_airports.h       text eol=crlf`nunrelated.txt text eol=lf"
    Expect-Fail "header lacks same-line LF rule" $pyOrig $hOrig $cppOrig $badAttr7

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

    # T13: CRLF normalization would hide generated-file line-ending drift.
    $badPy13 = $pyOrig -replace [regex]::Escape("on_disk = path.read_bytes()"),
      'on_disk = path.read_bytes().replace(b"\r\n", b"\n")'
    Expect-Fail "CRLF normalization in check mode" $badPy13 $hOrig $cppOrig $attrOrig

    # T14: a generated runway endpoint outside geographic bounds is rejected.
    $firstEndpointLine = ($cppOrig -split "`n" | Where-Object {
      $_ -match '^\s*\{\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*\d+\},'
    } | Select-Object -First 1)
    $badCpp14 = $cppOrig -replace [regex]::Escape($firstEndpointLine),
      ($firstEndpointLine -replace '^\s*\{(\d+),\s*-?\d+', '  {$1, 900000001')
    Expect-Fail "out-of-range runway endpoint" $pyOrig $hOrig $badCpp14 $attrOrig

    # T15: a changed live URL path must fail even though the pinned commit string
    # still appears elsewhere (base + _COMMIT are unchanged; only the suffix moved).
    $badPy15 = $pyOrig -replace [regex]::Escape('AIRPORTS_URL = _BASE_URL + "/airports.csv"'),
      'AIRPORTS_URL = _BASE_URL + "/data/airports.csv"'
    Expect-Fail "changed live airports URL path" $badPy15 $hOrig $cppOrig $attrOrig

    # T16: the CAPTURED generator SHA must be lowercase 64-hex -- an uppercase
    # 64-char value (right length, wrong case) is rejected on format.
    $badPy16 = $pyOrig -replace "092223c8d6a1cf60c13d450e61a91438cc80c5fd50f92f52f49a38826e04a354",
      "092223C8D6A1CF60C13D450E61A91438CC80C5FD50F92F52F49A38826E04A354"
    Expect-Fail "uppercase (non-lowercase-hex) generator SHA" $badPy16 $hOrig $cppOrig $attrOrig

    # T17: a verifier that computes hashlib.sha256 but no longer COMPARES it to
    # expected_sha256 must fail (occurrence of hashlib.sha256 is insufficient).
    $badPy17 = $pyOrig -replace [regex]::Escape("actual_sha != expected_sha256"), "actual_sha != actual_sha"
    Expect-Fail "verifier SHA comparison bypass" $badPy17 $hOrig $cppOrig $attrOrig

    # T18: if the check_mode body cannot be located the gate must FAIL (never
    # silently skip its body checks).
    $badPy18 = $pyOrig -replace [regex]::Escape("def check_mode("), "def check_mode_renamed("
    Expect-Fail "check_mode body not locatable" $badPy18 $hOrig $cppOrig $attrOrig

    # T19: verifier-looking docstring text cannot replace executable checks.
    $badPy19 = $pyOrig -replace [regex]::Escape("    actual_len = len(data)"), ('    """actual_len = len(data)' + "`n" + '    if actual_len != expected_length:' + "`n" + '    actual_sha = hashlib.sha256(data).hexdigest()' + "`n" + '    if actual_sha != expected_sha256:' + "`n" + '    """')
    Expect-Fail "verifier docstring bypass" $badPy19 $hOrig $cppOrig $attrOrig
    # T20: check_mode cannot borrow a later numeric return from main.
    $badPy20 = $pyOrig -replace [regex]::Escape("    return 0 if ok else 1"), "    pass"
    Expect-Fail "check_mode missing own return" $badPy20 $hOrig $cppOrig $attrOrig
    # T21: _BASE_URL must end at the parenthesized pinned expression.
    $badPy21 = $pyOrig -replace '(?m)(\s+\+ _COMMIT\r?\n\))', '$1 + "/evil"'
    Expect-Fail "extended base URL expression" $badPy21 $hOrig $cppOrig $attrOrig
    # T22: identity encoding must be the actual Request header.
    $badPy22 = $pyOrig -replace [regex]::Escape('headers={"Accept-Encoding": "identity"}'), 'headers={}'
    Expect-Fail "missing Request identity header" $badPy22 $hOrig $cppOrig $attrOrig
    # T23: help text cannot replace paired local-argument enforcement.
    $badPy23 = $pyOrig -replace [regex]::Escape("if bool(local_airports) != bool(local_runways):"), "if False:"
    Expect-Fail "missing paired local-argument enforcement" $badPy23 $hOrig $cppOrig $attrOrig
    # T24: generic write_bytes/encode text cannot replace exact LF writes.
    $badPy24 = $pyOrig -replace [regex]::Escape('OUT_H.write_bytes(header.encode("utf-8"))'), 'OUT_H.write_bytes(header.encode("ascii"))'
    Expect-Fail "non-deterministic header write encoding" $badPy24 $hOrig $cppOrig $attrOrig
    # T25: an exact-looking write inside an ordinary string cannot replace the
    # executable header write statement.
    $badPy25 = $pyOrig -replace [regex]::Escape('OUT_H.write_bytes(header.encode("utf-8"))'),
      ("OUT_H.write_bytes(header.encode(`"ascii`"))" + "`n    'OUT_H.write_bytes(header.encode(`"utf-8`"))'")
    Expect-Fail "header write hidden in string literal" $badPy25 $hOrig $cppOrig $attrOrig

    Write-Host ""
    Write-Host "OK: all 25 self-test tamper cases correctly rejected."
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
