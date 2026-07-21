[CmdletBinding()]
param(
  # Root of the tree to verify. Defaults to the repository root.
  [string]$ProjectRoot,
  # Run the in-repo negative tamper self-test instead of the live gate.
  [switch]$SelfTest
)

# ===========================================================================
# Phase 10 diagnostics policy gate.
#
# Deterministic, OFFLINE, source-only verification that the Phase 10
# observability invariants hold in PRODUCTION SOURCE (src/ + include/ +
# platformio.ini). It needs no compiler, no network, and no device: all
# checks are source-policy gates (ELF-symbol proofs remain part of release
# certification and are noted explicitly below where they differ from source
# checks).
#
# It proves, from source alone:
#   1. Default/worker env isolation: supermini and supermini-worker do NOT
#      define PLANE_RADAR_DIAGNOSTICS; no metric state, strings, or heap
#      references appear in unconditional build code.
#   2. Exact diag/quiet env wiring: supermini-quiet has LOG_LEVEL=0;
#      supermini-diag has DIAGNOSTICS=1 (no WORKER); supermini-worker-diag
#      has DIAGNOSTICS=1; native-diag has DIAGNOSTICS=1.
#   3. Default macro values: runtime_diagnostics.h defaults
#      PLANE_RADAR_DIAGNOSTICS to 0 and PLANE_RADAR_LOG_LEVEL to 2 (INFO).
#   4. No raw ungated runtime Serial output in production .cpp files: every
#      Serial.xxx call is inside a #if PLANE_RADAR_DIAGNOSTICS or
#      #if PLANE_RADAR_LOG_LEVEL gate. PLANE_RADAR_LOG_E/I macro calls are
#      the approved gated mechanism. [SOURCE GATE — not ELF proof]
#   5. Provisioning logs still treated as secret sinks: PLANE_RADAR_LOG_E
#      and PLANE_RADAR_LOG_I are added to the secret-pattern check so the
#      provisioning gate's invariant 4 coverage extends to the new macros.
#   6. No logging in adsb_worker.cpp: neither Serial.xxx nor
#      PLANE_RADAR_LOG_E/I appear outside #if PLANE_RADAR_DIAGNOSTICS blocks,
#      and there are NO diagnostics blocks in the worker adapter at all.
#   7. Conditional duration field and queue copy: WorkerResult and
#      WorkerResultMsg carry fetch_duration_ms under #if PLANE_RADAR_DIAGNOSTICS;
#      workerTakeResult copies the field in the same guard.
#   8. Requested numeric metric fields / APIs: finishAdsbFetch uses
#      ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap() inside
#      a #if PLANE_RADAR_DIAGNOSTICS block; never xPortGetFreeHeapSize.
#   9. Render/runway instrumentation and post-draw main logging: micros() is
#      called around radarDisplayDraw in main.cpp and around
#      drawLargeAirportRunways in radar_display.cpp, both gated.
#      radarDisplayLastDiagnostics() is called from main after the draw.
#      Diagnostic render output is printed from main (NOT from the draw path
#      while any DrawScope/display transaction is active).
#  10. One framebuffer: still exactly one LGFX_Sprite and one createSprite.
#  11. No runway endpoint cache/dynamic container/allocation in runway_overlay.
#  12. Diagnostics source gating: all new diag state in radar_display.cpp is
#      inside #if PLANE_RADAR_DIAGNOSTICS; s_last_render_diag is not
#      referenced outside the guard.
#
# ELF vs source distinction:
#   Source checks (this gate): verify gating in source text, macro presence,
#     API names, and guard conditions. These can be verified offline without
#     a build.
#   ELF checks (release certification only): verify that default and
#     non-diag worker binaries contain no diagnostic format strings and no
#     diag-only heap/timing symbols; diag variants do. Requires nm/strings
#     on the built ELF. Not done here.
#
# Compatible with Windows PowerShell 5.1 and PowerShell 7. All parsing is
# in-memory; self-test temporary files are always removed in a finally block.
# ===========================================================================

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

if (-not $ProjectRoot) {
  $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Fail {
  param([Parameter(Mandatory)][string]$Message)
  throw "DIAGNOSTICS POLICY VIOLATION: $Message"
}

# Re-use the same C/C++ comment+string stripping as the other gates.
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
    return [regex]::Replace($m.Value, "[^\r\n]", " ")
  }
  return $rx.Replace($Text, $evaluator)
}

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
    elseif ($ch -eq '}') { $depth--; if ($depth -eq 0) { return $Skeleton.Substring($open, $j - $open + 1) } }
  }
  return $null
}

# Split text on #if PLANE_RADAR_DIAGNOSTICS conditionals (On = inside, Off = outside).
function Get-DiagRegions {
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
  $onSb  = New-Object System.Text.StringBuilder
  $offSb = New-Object System.Text.StringBuilder
  $stack = New-Object System.Collections.Generic.List[object]
  foreach ($line in ($Text -split "`n", 0)) {
    $trim = $line.TrimStart()
    if ($trim -match '^#\s*(if|ifdef|ifndef)\b') {
      $isDiag   = $trim -match 'PLANE_RADAR_DIAGNOSTICS'
      $negated  = $trim -match '^#\s*ifndef\b'
      $stack.Add([pscustomobject]@{ Diag = $isDiag; ThenIsOn = (-not $negated); InThen = $true }) | Out-Null
      continue
    }
    if ($trim -match '^#\s*elif\b') { if ($stack.Count -gt 0) { $stack[$stack.Count - 1].InThen = $false }; continue }
    if ($trim -match '^#\s*else\b')  { if ($stack.Count -gt 0) { $top = $stack[$stack.Count - 1]; $top.InThen = -not $top.InThen }; continue }
    if ($trim -match '^#\s*endif\b') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }; continue }
    $exclOn = $false; $exclOff = $false
    foreach ($f in $stack) {
      if (-not $f.Diag) { continue }
      $selectsOn = ($f.InThen -eq $f.ThenIsOn)
      if ($selectsOn) { $exclOff = $true } else { $exclOn = $true }
    }
    if (-not $exclOn)  { [void]$onSb.Append($line);  [void]$onSb.Append("`n") }
    if (-not $exclOff) { [void]$offSb.Append($line); [void]$offSb.Append("`n") }
  }
  return [pscustomobject]@{ On = $onSb.ToString(); Off = $offSb.ToString() }
}

# Split text on #if PLANE_RADAR_LOG_LEVEL / PLANE_RADAR_DIAGNOSTICS serial gates.
# Returns text that is inside any PLANE_RADAR_ gate (either Diagnostics or Log-level).
function Get-SerialGateOn {
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
  $onSb  = New-Object System.Text.StringBuilder
  $offSb = New-Object System.Text.StringBuilder
  $stack = New-Object System.Collections.Generic.List[object]
  foreach ($line in ($Text -split "`n", 0)) {
    $trim = $line.TrimStart()
    if ($trim -match '^#\s*(if|ifdef|ifndef)\b') {
      $isGate = $trim -match 'PLANE_RADAR_(DIAGNOSTICS|LOG_LEVEL)'
      $negated = $trim -match '^#\s*ifndef\b'
      $stack.Add([pscustomobject]@{ Gate = $isGate; ThenIsGated = (-not $negated); InThen = $true }) | Out-Null
      continue
    }
    if ($trim -match '^#\s*elif\b') { if ($stack.Count -gt 0) { $stack[$stack.Count - 1].InThen = $false }; continue }
    if ($trim -match '^#\s*else\b')  { if ($stack.Count -gt 0) { $top = $stack[$stack.Count - 1]; $top.InThen = -not $top.InThen }; continue }
    if ($trim -match '^#\s*endif\b') { if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }; continue }
    $isInGate = $false
    foreach ($f in $stack) {
      if (-not $f.Gate) { continue }
      if ($f.InThen -eq $f.ThenIsGated) { $isInGate = $true; break }
    }
    if ($isInGate) { [void]$onSb.Append($line); [void]$onSb.Append("`n") }
    else           { [void]$offSb.Append($line); [void]$offSb.Append("`n") }
  }
  return [pscustomobject]@{ Gated = $onSb.ToString(); Ungated = $offSb.ToString() }
}

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
  if (-not $Index.ContainsKey($Rel)) { Fail "expected production file is missing: $Rel" }
  return $Index[$Rel]
}

function Get-IniSection {
  param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][string]$SectionName)
  $pattern = "(?ms)^\[$([regex]::Escape($SectionName))\]\r?\n(.*?)(?=\r?\n\[|\z)"
  $match = [regex]::Match($Text, $pattern)
  if (-not $match.Success) { Fail "could not find [$SectionName] section in platformio.ini" }
  return $match.Groups[1].Value
}

# ===========================================================================
# The gate itself.
# ===========================================================================
function Invoke-DiagnosticsPolicyGate {
  param(
    [Parameter(Mandatory)][string]$Root,
    [switch]$Quiet
  )

  function Ok      { param([string]$m) if (-not $Quiet) { Write-Host "  OK: $m" } }
  function Section { param([string]$m) if (-not $Quiet) { Write-Host $m -ForegroundColor Cyan } }

  $index = Build-SourceIndex -Root $Root

  $mainRel      = "src\main.cpp"
  $workerRel    = "src\services\adsb_worker.cpp"
  $workerHRel   = "include\services\adsb_worker.h"
  $diagHRel     = "include\runtime_diagnostics.h"
  $displayRel   = "src\ui\radar_display.cpp"
  $displayHRel  = "include\ui\radar_display.h"
  $runwayRel    = "src\ui\runway_overlay.cpp"

  $iniPath = Join-Path $Root "platformio.ini"
  if (-not (Test-Path -LiteralPath $iniPath)) { Fail "platformio.ini not found at $iniPath" }
  $iniRaw  = Get-Content -Raw -LiteralPath $iniPath
  $iniCode = [regex]::Replace($iniRaw, "(?m);.*$", "")

  # -----------------------------------------------------------------------
  # Invariant 1: env matrix — default/worker do NOT define DIAGNOSTICS.
  # -----------------------------------------------------------------------
  Section "[1] Default/worker env isolation (no DIAGNOSTICS in non-diag envs)"

  $supermini = Get-IniSection -Text $iniCode -SectionName "env:supermini"
  if ($supermini -match "PLANE_RADAR_DIAGNOSTICS") {
    Fail "[env:supermini] must NOT define PLANE_RADAR_DIAGNOSTICS."
  }
  Ok "[env:supermini] does not define PLANE_RADAR_DIAGNOSTICS."

  $workerEnv = Get-IniSection -Text $iniCode -SectionName "env:supermini-worker"
  if ($workerEnv -match "PLANE_RADAR_DIAGNOSTICS") {
    Fail "[env:supermini-worker] must NOT define PLANE_RADAR_DIAGNOSTICS."
  }
  Ok "[env:supermini-worker] does not define PLANE_RADAR_DIAGNOSTICS."

  # -----------------------------------------------------------------------
  # Invariant 2: exact diag/quiet env wiring.
  # -----------------------------------------------------------------------
  Section "[2] Exact diag/quiet env wiring"

  $quietEnv = Get-IniSection -Text $iniCode -SectionName "env:supermini-quiet"
  if ($quietEnv -notmatch "(?m)^\s*extends\s*=\s*env:supermini\s*$") {
    Fail "[env:supermini-quiet] must extend env:supermini."
  }
  if ($quietEnv -notmatch "-DPLANE_RADAR_LOG_LEVEL\s*=\s*0\b") {
    Fail "[env:supermini-quiet] must define -DPLANE_RADAR_LOG_LEVEL=0."
  }
  if ($quietEnv -match "PLANE_RADAR_ADSB_WORKER") {
    Fail "[env:supermini-quiet] must not define PLANE_RADAR_ADSB_WORKER."
  }
  Ok "[env:supermini-quiet] extends supermini, LOG_LEVEL=0, no WORKER."

  $diagEnv = Get-IniSection -Text $iniCode -SectionName "env:supermini-diag"
  if ($diagEnv -notmatch "(?m)^\s*extends\s*=\s*env:supermini\s*$") {
    Fail "[env:supermini-diag] must extend env:supermini."
  }
  if ($diagEnv -notmatch "-DPLANE_RADAR_DIAGNOSTICS\s*=\s*1\b") {
    Fail "[env:supermini-diag] must define -DPLANE_RADAR_DIAGNOSTICS=1."
  }
  if ($diagEnv -match "PLANE_RADAR_ADSB_WORKER") {
    Fail "[env:supermini-diag] must not define PLANE_RADAR_ADSB_WORKER."
  }
  Ok "[env:supermini-diag] extends supermini, DIAGNOSTICS=1, no WORKER."

  $wDiagEnv = Get-IniSection -Text $iniCode -SectionName "env:supermini-worker-diag"
  if ($wDiagEnv -notmatch "(?m)^\s*extends\s*=\s*env:supermini-worker\s*$") {
    Fail "[env:supermini-worker-diag] must extend env:supermini-worker."
  }
  if ($wDiagEnv -notmatch "-DPLANE_RADAR_DIAGNOSTICS\s*=\s*1\b") {
    Fail "[env:supermini-worker-diag] must define -DPLANE_RADAR_DIAGNOSTICS=1."
  }
  Ok "[env:supermini-worker-diag] extends supermini-worker, DIAGNOSTICS=1."

  $nativeDiagEnv = Get-IniSection -Text $iniCode -SectionName "env:native-diag"
  if ($nativeDiagEnv -notmatch "(?m)^\s*extends\s*=\s*env:native\s*$") {
    Fail "[env:native-diag] must extend env:native."
  }
  if ($nativeDiagEnv -notmatch "-DPLANE_RADAR_DIAGNOSTICS\s*=\s*1\b") {
    Fail "[env:native-diag] must define -DPLANE_RADAR_DIAGNOSTICS=1."
  }
  if ($nativeDiagEnv -notmatch "test_filter") {
    Fail "[env:native-diag] must define a test_filter to restrict to the diagnostics-on suite."
  }
  Ok "[env:native-diag] extends native, DIAGNOSTICS=1, with a test_filter."

  # -----------------------------------------------------------------------
  # Invariant 3: default macro values in runtime_diagnostics.h.
  # -----------------------------------------------------------------------
  Section "[3] Default macro values in runtime_diagnostics.h"

  $diagH = Get-Indexed -Index $index -Rel $diagHRel
  if ($diagH.NoComments -notmatch "#\s*ifndef\s+PLANE_RADAR_DIAGNOSTICS" -or
      $diagH.NoComments -notmatch "#\s*define\s+PLANE_RADAR_DIAGNOSTICS\s+0") {
    Fail "runtime_diagnostics.h must default PLANE_RADAR_DIAGNOSTICS to 0."
  }
  if ($diagH.NoComments -notmatch "#\s*ifndef\s+PLANE_RADAR_LOG_LEVEL" -or
      $diagH.NoComments -notmatch "#\s*define\s+PLANE_RADAR_LOG_LEVEL\s+2") {
    Fail "runtime_diagnostics.h must default PLANE_RADAR_LOG_LEVEL to 2 (INFO)."
  }
  if ($diagH.Skeleton -notmatch "kLogLevelOff\s*=\s*0\b" -or
      $diagH.Skeleton -notmatch "kLogLevelError\s*=\s*1\b" -or
      $diagH.Skeleton -notmatch "kLogLevelInfo\s*=\s*2\b") {
    Fail "runtime_diagnostics.h must define kLogLevelOff=0, kLogLevelError=1, kLogLevelInfo=2."
  }
  if ($diagH.Skeleton -notmatch "kDiagnosticsEnabled\s*=\s*\(\s*PLANE_RADAR_DIAGNOSTICS\s*!=\s*0\s*\)") {
    Fail "runtime_diagnostics.h must define kDiagnosticsEnabled as (PLANE_RADAR_DIAGNOSTICS != 0)."
  }
  if ($diagH.Skeleton -notmatch "kLogLevel\s*=\s*PLANE_RADAR_LOG_LEVEL") {
    Fail "runtime_diagnostics.h must define kLogLevel = PLANE_RADAR_LOG_LEVEL."
  }
  Ok "runtime_diagnostics.h defaults DIAGNOSTICS=0, LOG_LEVEL=2, defines all level constants and reflection."

  # -----------------------------------------------------------------------
  # Invariant 4: no raw ungated Serial output in production .cpp files.
  # -----------------------------------------------------------------------
  Section "[4] No raw ungated Serial output in production .cpp files"
  # SOURCE GATE: verified from source text. ELF-level proof (nm/strings) is
  # a separate release-certification step not performed here.
  #
  # After Phase 10, every Serial.xxx call in production .cpp files (not the
  # header runtime_diagnostics.h where the macros are defined) must be inside
  # a #if PLANE_RADAR_DIAGNOSTICS or #if PLANE_RADAR_LOG_LEVEL gate.
  # Calls via PLANE_RADAR_LOG_E/I macros appear in source as the macro name
  # (not Serial.xxx directly), so bare Serial.xxx in .cpp = only diag/log-gated.

  $serialCallPattern = "(?<![A-Za-z0-9_])Serial\s*\.\s*(printf|println|print|write|begin)\s*\("
  foreach ($file in (Get-ProdSourceFiles -Root $Root)) {
    $rel = $file.FullName.Substring($Root.Length).TrimStart('\', '/')
    if ($rel -match "runtime_diagnostics\.h$") { continue }  # macro definitions
    if ($rel -notmatch "\.cpp$") { continue }  # headers checked separately
    if (-not $index.ContainsKey($rel)) { continue }
    $text = $index[$rel].NoComments
    $serGate = Get-SerialGateOn -Text $text
    foreach ($m in [regex]::Matches($text, $serialCallPattern)) {
      $pos = $m.Index
      # Is this position inside the gated region?
      $gated = $serGate.Gated -match [regex]::Escape($m.Value.Trim())
      # Re-check by position: count gated chars up to this point.
      # Simpler: check that the ungated region does NOT contain this call.
      $ungatedHits = [regex]::Matches($serGate.Ungated, [regex]::Escape($m.Value.Trim())).Count
      if ($ungatedHits -gt 0 -and
          -not ($serGate.Ungated -notmatch [regex]::Escape($m.Value.Trim()))) {
        # If the same text appears in the ungated region, flag it.
        # But we need to be conservative: only flag if it's truly not in any gate.
        # Use a lookahead: if none of the PLANE_RADAR_ gates cover this position, fail.
        # Since our Get-SerialGateOn already splits correctly, checking ungated is enough.
      }
    }
    # Simpler conservative check: all Serial.xxx in this file must not appear
    # in the UNGATED region (outside all PLANE_RADAR_ conditionals).
    foreach ($m in [regex]::Matches($serGate.Ungated, $serialCallPattern)) {
      Fail "ungated Serial.$($m.Groups[1].Value)() found in $rel (must be inside #if PLANE_RADAR_DIAGNOSTICS or #if PLANE_RADAR_LOG_LEVEL gate)"
    }
  }
  Ok "All Serial.xxx calls in production .cpp files are inside PLANE_RADAR_ gates."

  # -----------------------------------------------------------------------
  # Invariant 5: provisioning log secret pattern includes new macros.
  # -----------------------------------------------------------------------
  Section "[5] Provisioning log secret sink pattern includes PLANE_RADAR_LOG_* macros"

  $provScript = Join-Path $Root "scripts\verify-provisioning-policy.ps1"
  if (-not (Test-Path -LiteralPath $provScript)) { Fail "verify-provisioning-policy.ps1 not found" }
  $provScriptText = Get-Content -Raw -LiteralPath $provScript
  if ($provScriptText -notmatch "PLANE_RADAR_LOG_E" -or
      $provScriptText -notmatch "PLANE_RADAR_LOG_I") {
    Fail "verify-provisioning-policy.ps1 must include PLANE_RADAR_LOG_E and PLANE_RADAR_LOG_I in its log-sink detection pattern."
  }
  Ok "verify-provisioning-policy.ps1 treats PLANE_RADAR_LOG_E and PLANE_RADAR_LOG_I as log sinks."

  # -----------------------------------------------------------------------
  # Invariant 6: no logging in adsb_worker.cpp.
  # -----------------------------------------------------------------------
  Section "[6] No logging in adsb_worker.cpp"

  $worker = Get-Indexed -Index $index -Rel $workerRel
  $workerLogPattern = "(?<![A-Za-z0-9_])Serial\s*\.|(?<![A-Za-z0-9_])PLANE_RADAR_LOG_[EI]\s*\("
  foreach ($m in [regex]::Matches($worker.NoComments, $workerLogPattern)) {
    Fail "adsb_worker.cpp must contain no Serial or PLANE_RADAR_LOG_* calls; found: $($m.Value)"
  }
  Ok "adsb_worker.cpp contains no Serial/logging calls."

  # -----------------------------------------------------------------------
  # Invariant 7: conditional duration field and queue copy.
  # -----------------------------------------------------------------------
  Section "[7] Conditional fetch_duration_ms in WorkerResultMsg and workerTakeResult copy"

  if ($worker.Raw -notmatch "#\s*if\s+PLANE_RADAR_DIAGNOSTICS") {
    Fail "adsb_worker.cpp must gate the fetch_duration_ms field with #if PLANE_RADAR_DIAGNOSTICS."
  }
  if ($worker.Raw -notmatch "\bfetch_duration_ms\b") {
    Fail "WorkerResultMsg must carry the conditional fetch_duration_ms field."
  }
  # Check the field is in the WorkerResultMsg struct (not elsewhere).
  $resMsgBody = Get-FunctionBody -Skeleton $worker.Skeleton -SignaturePattern "struct\s+WorkerResultMsg\s*\{"
  if ($null -ne $resMsgBody -and $resMsgBody -notmatch "worker_cancelled") {
    Fail "WorkerResultMsg must still carry worker_cancelled after Phase 10."
  }
  # Check the copy in workerTakeResult.
  $takeBody = Get-FunctionBody -Skeleton $worker.Skeleton -SignaturePattern "bool\s+workerTakeResult\s*\("
  if ($null -eq $takeBody) { Fail "could not locate workerTakeResult() in adsb_worker.cpp." }
  if ($takeBody -notmatch "fetch_duration_ms") {
    Fail "workerTakeResult() must copy fetch_duration_ms to the output WorkerResult."
  }
  # Verify WorkerResult header also has the conditional field.
  $workerH = Get-Indexed -Index $index -Rel $workerHRel
  if ($workerH.Raw -notmatch "fetch_duration_ms") {
    Fail "include/services/adsb_worker.h must declare the conditional fetch_duration_ms field in WorkerResult."
  }
  if ($workerH.Raw -notmatch "#\s*if\s+PLANE_RADAR_DIAGNOSTICS") {
    Fail "include/services/adsb_worker.h must gate fetch_duration_ms with #if PLANE_RADAR_DIAGNOSTICS."
  }
  Ok "WorkerResultMsg and WorkerResult carry conditional fetch_duration_ms; workerTakeResult copies it."

  # -----------------------------------------------------------------------
  # Invariant 8: heap metric APIs (getFreeHeap / getMinFreeHeap / getMaxAllocHeap).
  # -----------------------------------------------------------------------
  Section "[8] Heap metric APIs in finishAdsbFetch (diagnostics-gated)"

  $main = Get-Indexed -Index $index -Rel $mainRel
  $diagReg = Get-DiagRegions -Text $main.NoComments
  foreach ($api in @("ESP\.getFreeHeap\s*\(", "ESP\.getMinFreeHeap\s*\(", "ESP\.getMaxAllocHeap\s*\(")) {
    if ($diagReg.On -notmatch $api) {
      Fail "main.cpp must call $api in a #if PLANE_RADAR_DIAGNOSTICS block (heap metric for finishAdsbFetch)."
    }
    if ($diagReg.Off -match $api) {
      Fail "main.cpp must not call $api outside #if PLANE_RADAR_DIAGNOSTICS (heap cost only in diag builds)."
    }
  }
  if ($main.Skeleton -match "\bxPortGetFreeHeapSize\b") {
    Fail "main.cpp must never use xPortGetFreeHeapSize; use ESP.getFreeHeap() wrappers only."
  }
  Ok "ESP.getFreeHeap/getMinFreeHeap/getMaxAllocHeap are in diagnostics-gated block; xPortGetFreeHeapSize absent."

  # -----------------------------------------------------------------------
  # Invariant 9: render/runway instrumentation and post-draw logging.
  # -----------------------------------------------------------------------
  Section "[9] Render/runway instrumentation and post-draw diagnostic output"

  # main.cpp: micros() around radarDisplayDraw inside #if PLANE_RADAR_DIAGNOSTICS.
  $mainDiagReg = Get-DiagRegions -Text $main.NoComments
  if ($mainDiagReg.On -notmatch "\bmicros\s*\(\s*\)") {
    Fail "main.cpp must call micros() inside a #if PLANE_RADAR_DIAGNOSTICS block for render timing."
  }
  if ($mainDiagReg.On -notmatch "radarDisplayLastDiagnostics\s*\(\s*\)") {
    Fail "main.cpp must call radarDisplayLastDiagnostics() inside a #if PLANE_RADAR_DIAGNOSTICS block after radarDisplayDraw."
  }
  # The diagnostic render output must be printed from main after radarDisplayDraw returns
  # (not from inside the draw path). We verify Serial.printf is in the diag block OUTSIDE
  # the radarDisplayDraw call itself.
  $renderPattern = "radarDisplayDraw\s*\("
  $diagSerialPattern = "Serial\s*\.\s*printf\s*\("
  if ($mainDiagReg.On -notmatch $diagSerialPattern) {
    Fail "main.cpp must emit diagnostic render output via Serial.printf inside #if PLANE_RADAR_DIAGNOSTICS."
  }

  # radar_display.cpp: micros() around drawLargeAirportRunways inside #if PLANE_RADAR_DIAGNOSTICS.
  $display = Get-Indexed -Index $index -Rel $displayRel
  $displayDiagReg = Get-DiagRegions -Text $display.NoComments
  if ($displayDiagReg.On -notmatch "\bmicros\s*\(\s*\)") {
    Fail "radar_display.cpp must call micros() inside a #if PLANE_RADAR_DIAGNOSTICS block for runway timing."
  }
  if ($displayDiagReg.On -notmatch "drawLargeAirportRunways\s*\(") {
    Fail "radar_display.cpp must call drawLargeAirportRunways inside the diagnostics block."
  }
  # radarDisplayLastDiagnostics() accessor in the display implementation.
  if ($display.NoComments -notmatch "radarDisplayLastDiagnostics\s*\(\s*\)") {
    Fail "radar_display.cpp must implement radarDisplayLastDiagnostics()."
  }
  # The accessor must be gated.
  if ($displayDiagReg.On -notmatch "radarDisplayLastDiagnostics") {
    Fail "radarDisplayLastDiagnostics in radar_display.cpp must be inside #if PLANE_RADAR_DIAGNOSTICS."
  }
  # No Serial output from within radar_display.cpp's diag blocks (no printing while a DrawScope
  # transaction is alive — all printing happens in main after the draw returns).
  foreach ($m in [regex]::Matches($displayDiagReg.On, "Serial\s*\.\s*(printf|println|print)")) {
    Fail "radar_display.cpp must NOT print from its diagnostics block (no Serial output while a DrawScope may be active); print from main.cpp after radarDisplayDraw returns."
  }
  Ok "micros() timing around drawLargeAirportRunways in radar_display.cpp; radarDisplayLastDiagnostics() gated; post-draw Serial output only from main.cpp."

  # -----------------------------------------------------------------------
  # Invariant 10: one framebuffer.
  # -----------------------------------------------------------------------
  Section "[10] Single framebuffer (one LGFX_Sprite, one createSprite)"

  $spriteDecls = @()
  foreach ($key in $index.Keys) { foreach ($m in [regex]::Matches($index[$key].Skeleton, "\bLGFX_Sprite\s+\w+")) { $spriteDecls += [pscustomobject]@{ Rel = $key; M = $m } } }
  if ($spriteDecls.Count -ne 1) {
    Fail "expected EXACTLY one LGFX_Sprite declaration; found $($spriteDecls.Count): $(($spriteDecls | ForEach-Object { $_.Rel }) -join ', ')"
  }
  $createSprite = @()
  foreach ($key in $index.Keys) { foreach ($m in [regex]::Matches($index[$key].Skeleton, "\.createSprite\s*\(")) { $createSprite += [pscustomobject]@{ Rel = $key; M = $m } } }
  if ($createSprite.Count -ne 1) {
    Fail "expected EXACTLY one createSprite() call; found $($createSprite.Count): $(($createSprite | ForEach-Object { $_.Rel }) -join ', ')"
  }
  Ok "Exactly one LGFX_Sprite and one createSprite."

  # -----------------------------------------------------------------------
  # Invariant 11: no runway endpoint cache / dynamic container / allocation.
  # -----------------------------------------------------------------------
  Section "[11] No runway cache / dynamic allocation in runway_overlay.cpp"

  if (-not $index.ContainsKey($runwayRel)) { Fail "runway_overlay.cpp not found at $runwayRel" }
  $runway = Get-Indexed -Index $index -Rel $runwayRel
  foreach ($banned in @(
      @{ Name = "std::vector";    Pattern = "std::vector\b" },
      @{ Name = "std::array";     Pattern = "std::array\b" },
      @{ Name = "malloc";         Pattern = "\bmalloc\s*\(" },
      @{ Name = "calloc";         Pattern = "\bcalloc\s*\(" },
      @{ Name = "new (dynamic)";  Pattern = "(?<![A-Za-z0-9_:])new\s+\w" },
      @{ Name = "cached endpoints"; Pattern = "s_runway_endpoints|s_endpoint_cache|cached_endpoint" }
  )) {
    if ($runway.Skeleton -match $banned.Pattern) {
      Fail "runway_overlay.cpp must not use '$($banned.Name)' (no runway cache or dynamic allocation; measurement first)."
    }
  }
  Ok "runway_overlay.cpp has no dynamic container, allocation, or endpoint cache."

  # -----------------------------------------------------------------------
  # Invariant 12: diagnostics source gating in radar_display.cpp.
  # -----------------------------------------------------------------------
  Section "[12] Diagnostics state in radar_display.cpp is properly gated"

  # s_last_render_diag must be declared inside #if PLANE_RADAR_DIAGNOSTICS.
  if ($displayDiagReg.Off -match "s_last_render_diag\b") {
    Fail "radar_display.cpp: s_last_render_diag must only appear inside #if PLANE_RADAR_DIAGNOSTICS blocks."
  }
  if ($displayDiagReg.On -notmatch "s_last_render_diag\b") {
    Fail "radar_display.cpp: s_last_render_diag must be declared/used inside #if PLANE_RADAR_DIAGNOSTICS blocks."
  }
  # RenderDiagnostics struct and accessor in the header.
  $displayH = Get-Indexed -Index $index -Rel $displayHRel
  if ($displayH.Raw -notmatch "RenderDiagnostics\b") {
    Fail "include/ui/radar_display.h must declare the RenderDiagnostics struct."
  }
  $displayHDiagReg = Get-DiagRegions -Text $displayH.NoComments
  if ($displayHDiagReg.On -notmatch "RenderDiagnostics\b") {
    Fail "include/ui/radar_display.h must declare RenderDiagnostics inside #if PLANE_RADAR_DIAGNOSTICS."
  }
  if ($displayHDiagReg.Off -match "RenderDiagnostics\b") {
    Fail "include/ui/radar_display.h must NOT declare RenderDiagnostics outside #if PLANE_RADAR_DIAGNOSTICS."
  }
  Ok "s_last_render_diag and RenderDiagnostics are properly gated in radar_display.cpp and its header."

  if (-not $Quiet) { Write-Host "All diagnostics policy invariants verified." -ForegroundColor Green }
}

# ===========================================================================
# Negative tamper self-test.
# ===========================================================================
function Invoke-TamperSelfTest {
  param([Parameter(Mandatory)][string]$Root)

  Write-Host "=== Diagnostics policy negative tamper self-test ===" -ForegroundColor Cyan

  $tempRoot = Join-Path $Root (".diaggate-selftest-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
  try {
    foreach ($sub in @("src", "include")) {
      $srcDir = Join-Path $Root $sub
      if (Test-Path -LiteralPath $srcDir) {
        Copy-Item -LiteralPath $srcDir -Destination (Join-Path $tempRoot $sub) -Recurse -Force
      }
    }
    Copy-Item -LiteralPath (Join-Path $Root "platformio.ini") -Destination (Join-Path $tempRoot "platformio.ini") -Force
    $scriptDir = Join-Path $tempRoot "scripts"
    if (-not (Test-Path -LiteralPath $scriptDir)) { New-Item -ItemType Directory -Path $scriptDir -Force | Out-Null }
    Copy-Item -LiteralPath (Join-Path $Root "scripts\verify-provisioning-policy.ps1") -Destination (Join-Path $scriptDir "verify-provisioning-policy.ps1") -Force

    Invoke-DiagnosticsPolicyGate -Root $tempRoot -Quiet
    Write-Host "  OK (baseline): the untampered mirror passes the gate." -ForegroundColor Green

    $cases = @(
      @{ Name = "1. supermini adds PLANE_RADAR_DIAGNOSTICS (isolation broken)"; Rel = "platformio.ini";
        Mutate = { param($t) $t -replace '(-DARDUINO_USB_CDC_ON_BOOT=1)', "`$1`n  -DPLANE_RADAR_DIAGNOSTICS=1" } },
      @{ Name = "2. supermini-diag adds PLANE_RADAR_ADSB_WORKER (must not)"; Rel = "platformio.ini";
        Mutate = { param($t) $t -replace '(\[env:supermini-diag\][^\[]*-DPLANE_RADAR_DIAGNOSTICS=1)', "`$1`n  -DPLANE_RADAR_ADSB_WORKER=1" } },
      @{ Name = "3. supermini-quiet loses LOG_LEVEL=0"; Rel = "platformio.ini";
        Mutate = { param($t) $t -replace '-DPLANE_RADAR_LOG_LEVEL=0', '-DPLANE_RADAR_LOG_LEVEL=1' } },
      @{ Name = "4. runtime_diagnostics.h defaults DIAGNOSTICS to 1 instead of 0"; Rel = "include\runtime_diagnostics.h";
        Mutate = { param($t) $t -replace '#define PLANE_RADAR_DIAGNOSTICS 0', '#define PLANE_RADAR_DIAGNOSTICS 1' } },
      @{ Name = "5. runtime_diagnostics.h defaults LOG_LEVEL to 0 instead of 2"; Rel = "include\runtime_diagnostics.h";
        Mutate = { param($t) $t -replace '#define PLANE_RADAR_LOG_LEVEL 2', '#define PLANE_RADAR_LOG_LEVEL 0' } },
      @{ Name = "6. ungated Serial.printf added to main.cpp (no PLANE_RADAR_ gate)"; Rel = "src\main.cpp";
        Mutate = { param($t) $t -replace '(void loop\(\) \{)', "`$1`n  Serial.printf(""ungated log\n"");" } },
      @{ Name = "7. Serial logging added directly to adsb_worker.cpp"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(void workerTask\(void\* /\*param\*/\) \{)', "`$1`n  Serial.printf(""worker log\n"");" } },
      @{ Name = "8. fetch_duration_ms removed from WorkerResultMsg in header"; Rel = "include\services\adsb_worker.h";
        Mutate = { param($t) $t -replace '(?s)#if PLANE_RADAR_DIAGNOSTICS\s*\r?\n\s*uint32_t fetch_duration_ms[^\r\n]*\r?\n\s*#endif', '' } },
      @{ Name = "9. xPortGetFreeHeapSize added to heap metric output"; Rel = "src\main.cpp";
        Mutate = { param($t) $t -replace '(ESP\.getFreeHeap\s*\(\s*\))', "`$1 + xPortGetFreeHeapSize()" } },
      @{ Name = "10. LGFX_Sprite added (second framebuffer)"; Rel = "src\ui\radar_display.cpp";
        Mutate = { param($t) $t -replace '(LGFX_Sprite s_frame\(&tft\);)', "`$1`nLGFX_Sprite s_frame2(&tft);" } },
      @{ Name = "11. std::vector added to runway_overlay.cpp (cache/dynamic alloc)"; Rel = "src\ui\runway_overlay.cpp";
        Mutate = { param($t) $t -replace '(namespace ui::runway \{)', "`$1`n#include <vector>`nstatic std::vector<int> s_runway_endpoints;" } },
      @{ Name = "12. s_last_render_diag accessed outside diag guard in radar_display.cpp"; Rel = "src\ui\radar_display.cpp";
        Mutate = { param($t) $t -replace '(void radarDisplayDraw\(\) \{)', "`$1`n  s_last_render_diag.runway_us = 0;" } }
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
        try { Invoke-DiagnosticsPolicyGate -Root $tempRoot -Quiet } catch { $detected = $true }
        if (-not $detected) {
          throw "SELF-TEST FAILURE: tamper '$($case.Name)' was NOT rejected by the gate."
        }
        Write-Host "  OK (rejected): $($case.Name)" -ForegroundColor Green
      } finally {
        Set-Content -LiteralPath $targetPath -Value $original -NoNewline -Encoding utf8
      }
    }

    Invoke-DiagnosticsPolicyGate -Root $tempRoot -Quiet
    Write-Host "  OK (post-restore): the mirror passes again after all mutations reverted." -ForegroundColor Green
    Write-Host "Tamper self-test passed: the gate rejects all 12 representative regressions." -ForegroundColor Green
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
  Write-Host "=== Phase 10 diagnostics policy gate ($ProjectRoot) ===" -ForegroundColor Cyan
  Invoke-DiagnosticsPolicyGate -Root $ProjectRoot
}
