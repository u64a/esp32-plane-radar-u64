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
# Phase 9 optional ADS-B network-worker policy / tamper gate.
#
# Deterministic, OFFLINE, source-only verification that the Phase 9 optional
# network-worker invariants hold in PRODUCTION SOURCE (src/ + include/ +
# platformio.ini) -- never trusting comments, README, or build artifacts. It
# needs no compiler, no network and no device: it parses source only, so it is a
# pure source-policy gate (ELF-symbol proofs remain part of release
# certification, out of scope here). Every violation throws (non-zero exit).
#
# The architecture it gates (the ACTUAL design, not the obsolete mailbox one):
#   * include/services/adsb_worker.h + src/services/adsb_worker.cpp: the OPTIONAL
#     Arduino/FreeRTOS facade + one long-lived static worker task, compiled in
#     ONLY when PLANE_RADAR_ADSB_WORKER != 0 (the [env:supermini-worker] build).
#   * core::WorkerProtocolState + core::NetworkWorkIntentState: the two pure,
#     unit-tested cores (adsb_worker_protocol.*, network_work_intent.*).
#   * one xTaskCreateStatic, two depth-1 xQueueCreateStatic queues, one
#     16-byte-aligned 8192-byte StackType_t stack, priority 1.
#   * the worker fetches via services::adsb::fetchCandidateControlled and never
#     touches Wi-Fi/UI/NVS/publish; main owns publication + result pumping;
#     Configure/Erase latch through NetworkWorkIntent and wait for quiescence.
#   * exactly two AircraftSnapshots and one framebuffer sprite.
#
# It proves, from source alone, ten invariants:
#   1. Env matrix: default_envs = supermini; [env:supermini] never defines the
#      worker macro; exactly one [env:supermini-worker] extends env:supermini and
#      defines PLANE_RADAR_ADSB_WORKER=1.
#   2. Compile-gating: the heavy adapter, and every worker-specific state /
#      orchestration token in main.cpp + wifi_setup.cpp, live under
#      #if PLANE_RADAR_ADSB_WORKER; the default (worker-off) build has no worker
#      state (its inert stub carries no task/queue/stack).
#   3. Static allocation only: exactly one xTaskCreateStatic and exactly two
#      xQueueCreateStatic in the adapter; no dynamic task/queue/heap/STL nor any
#      heap-allocating FreeRTOS object creator (dynamic queue/semaphore/mutex/
#      event-group/timer/stream-buffer/message-buffer -- only the *Static twins
#      are allowed) in the worker + core files.
#   4. Fixed sizing: 8192-byte alignas(16) StackType_t stack (sizeof==1), priority
#      1, both queue depths 1, static TCB/queue buffers/backing storage, and no
#      task deletion/suspend/recreation.
#   5. Queue payloads are trivially-copyable metadata only (no snapshot / sprite /
#      credential / String / raw caller-stack pointer), carrying the required
#      generation / revision / epoch / CandidateResult / cancel flag.
#   6. Ownership: the adapter never publishes (publishCandidate); main pumps +
#      publishes; the worker uses fetchCandidateControlled + error-path
#      discardCandidate; no third AircraftSnapshot / second framebuffer sprite.
#   7. Worker isolation: adsb_worker.cpp calls no wifiLoop / WiFi / esp_wifi /
#      display / portal / settings / NVS / SNTP / cert-floor / cross-task stop,
#      and the cancel callback is a pure protocol query.
#   8. Main runtime contracts: results pumped before + after the top-level
#      wifiLoop() and on the display-ownership return paths; Quiesced discards +
#      adsbFetchAbortedForPause (never adsbFetchCompleted/publish); the empty-queue
#      path observes workerFaulted(); a fault halts dispatch with no sync fallback.
#   9. Provisioning contracts: the worker Configure/Erase button paths latch
#      requestConfigure/requestErase + pause (never immediate feed); the deferred
#      service's 'if (!nwQuiesced())' guard actually returns (bails out) before it
#      consumes the intent, so consumeIntent runs only on proven quiescence; Erase
#      supersedes Configure in the core; pause_adsb precedes same-action radio
#      changes; resume reuses the existing immediate-fetch latch.
#  10. No production leakage of PLANE_RADAR_NATIVE_TEST_ACCESS or a test-support
#      worker seam.
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
  throw "ADSB WORKER POLICY VIOLATION: $Message"
}

# --- C/C++ comment + string handling ---------------------------------------
# Return source with comments neutralised (replaced by whitespace, newlines kept
# so line/index math stays stable). With -BlankStrings the interior of every
# string/char literal is also blanked (quotes preserved) so brace matching and
# identifier/API scans are never fooled by text inside a literal (e.g. an
# #include path such as "freertos/queue.h" or a word inside a log message). Same
# convention as scripts/verify-provisioning-policy.ps1.
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

# Extract the balanced { ... } body of the first construct whose signature matches
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

# Split skeleton text into the code that survives when PLANE_RADAR_ADSB_WORKER is
# ON (!= 0) vs OFF (== 0). Only "#if PLANE_RADAR_ADSB_WORKER" conditionals steer
# the split; any OTHER #if/#ifdef/#ifndef is passthrough (its body inherits the
# surrounding worker classification) so nested unrelated conditionals cannot fool
# the projection. Preprocessor directive lines themselves are dropped from both
# projections so the macro name in the directive never matches a token scan.
# Unconditional lines land in BOTH projections. Returns { On ; Off }.
function Get-WorkerRegions {
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
  $onSb = New-Object System.Text.StringBuilder
  $offSb = New-Object System.Text.StringBuilder
  $stack = New-Object System.Collections.Generic.List[object]
  foreach ($line in ($Text -split "`n", 0)) {
    $trim = $line.TrimStart()
    if ($trim -match '^#\s*(if|ifdef|ifndef)\b') {
      $isWorker = $trim -match 'PLANE_RADAR_ADSB_WORKER'
      # #ifndef PLANE_RADAR_ADSB_WORKER would invert the sense; the gated .cpp
      # files use plain "#if PLANE_RADAR_ADSB_WORKER" (then-branch == ON), so a
      # worker frame starts in its ON (then) branch.
      $negated = $trim -match '^#\s*ifndef\b'
      $stack.Add([pscustomobject]@{ Worker = $isWorker; ThenIsOn = (-not $negated); InThen = $true }) | Out-Null
      continue
    }
    if ($trim -match '^#\s*elif\b') {
      if ($stack.Count -gt 0) { $stack[$stack.Count - 1].InThen = $false }
      continue
    }
    if ($trim -match '^#\s*else\b') {
      if ($stack.Count -gt 0) {
        $top = $stack[$stack.Count - 1]
        $top.InThen = -not $top.InThen
      }
      continue
    }
    if ($trim -match '^#\s*endif\b') {
      if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }
      continue
    }
    $excludedFromOn = $false
    $excludedFromOff = $false
    foreach ($f in $stack) {
      if (-not $f.Worker) { continue }
      # This worker frame currently selects its ON-branch when
      # (InThen == ThenIsOn); otherwise it selects its OFF-branch.
      $selectsOn = ($f.InThen -eq $f.ThenIsOn)
      if ($selectsOn) { $excludedFromOff = $true } else { $excludedFromOn = $true }
    }
    if (-not $excludedFromOn) { [void]$onSb.Append($line); [void]$onSb.Append("`n") }
    if (-not $excludedFromOff) { [void]$offSb.Append($line); [void]$offSb.Append("`n") }
  }
  return [pscustomobject]@{ On = $onSb.ToString(); Off = $offSb.ToString() }
}

# Drop preprocessor directive lines from skeleton text, leaving only code +
# blanked comments. Used for adjacency checks (e.g. that a pump statement is
# textually immediately before/after wifiLoop()) where the #if/#endif guards
# would otherwise interrupt the whitespace run between two statements.
function Get-CodeOnly {
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
  return [regex]::Replace($Text, "(?m)^\s*#.*$", "")
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

# Extract the [env:NAME] section body from comment-stripped platformio.ini text.
function Get-IniSection {
  param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][string]$SectionName)
  $pattern = "(?ms)^\[$([regex]::Escape($SectionName))\]\r?\n(.*?)(?=\r?\n\[|\z)"
  $match = [regex]::Match($Text, $pattern)
  if (-not $match.Success) {
    Fail "could not find [$SectionName] section in platformio.ini"
  }
  return $match.Groups[1].Value
}

# Region of a switch case from its label up to (and including) the first break;.
function Get-CaseRegion {
  param([Parameter(Mandatory)][string]$Body, [Parameter(Mandatory)][string]$LabelPattern)
  $m = [regex]::Match($Body, $LabelPattern)
  if (-not $m.Success) { return $null }
  $brk = $Body.IndexOf("break;", $m.Index)
  if ($brk -lt 0) { $brk = [Math]::Min($m.Index + 600, $Body.Length - 1) }
  return $Body.Substring($m.Index, $brk - $m.Index + 6)
}

# ===========================================================================
# The gate itself.
# ===========================================================================
function Invoke-WorkerPolicyGate {
  param(
    [Parameter(Mandatory)][string]$Root,
    [switch]$Quiet
  )

  function Ok { param([string]$m) if (-not $Quiet) { Write-Host "  OK: $m" } }
  function Section { param([string]$m) if (-not $Quiet) { Write-Host $m -ForegroundColor Cyan } }

  $index = Build-SourceIndex -Root $Root

  $workerRel  = "src\services\adsb_worker.cpp"
  $workerHRel = "include\services\adsb_worker.h"
  $protoCRel  = "src\core\adsb_worker_protocol.cpp"
  $protoHRel  = "include\core\adsb_worker_protocol.h"
  $intentCRel = "src\core\network_work_intent.cpp"
  $intentHRel = "include\core\network_work_intent.h"
  $mainRel    = "src\main.cpp"
  $wifiRel    = "src\services\wifi_setup.cpp"
  $storeRel   = "include\services\adsb_snapshot_store.h"
  $displayRel = "src\ui\radar_display.cpp"

  $worker = Get-Indexed -Index $index -Rel $workerRel
  $wReg   = Get-WorkerRegions -Text $worker.Skeleton

  # -----------------------------------------------------------------------
  # Invariant 1: environment matrix.
  # -----------------------------------------------------------------------
  Section "[1] Environment matrix (default off; one explicit worker env)"

  $iniPath = Join-Path $Root "platformio.ini"
  if (-not (Test-Path -LiteralPath $iniPath)) { Fail "platformio.ini not found at $iniPath" }
  $iniRaw = Get-Content -Raw -LiteralPath $iniPath
  $iniCode = [regex]::Replace($iniRaw, "(?m);.*$", "")   # strip ; comments

  if ($iniCode -notmatch "(?m)^\s*default_envs\s*=\s*supermini\s*$") {
    Fail "default_envs must be exactly 'supermini' (the worker-off firmware)."
  }
  Ok "default_envs = supermini (worker-off by default)."

  $supermini = Get-IniSection -Text $iniCode -SectionName "env:supermini"
  if ($supermini -match "PLANE_RADAR_ADSB_WORKER") {
    Fail "[env:supermini] must never reference PLANE_RADAR_ADSB_WORKER (the default build stays worker-off)."
  }
  Ok "[env:supermini] does not define the worker macro."

  $workerEnvCount = ([regex]::Matches($iniCode, "(?m)^\[env:supermini-worker\]")).Count
  if ($workerEnvCount -ne 1) {
    Fail "expected EXACTLY one [env:supermini-worker]; found $workerEnvCount."
  }
  $workerEnv = Get-IniSection -Text $iniCode -SectionName "env:supermini-worker"
  if ($workerEnv -notmatch "(?m)^\s*extends\s*=\s*env:supermini\s*$") {
    Fail "[env:supermini-worker] must 'extends = env:supermini'."
  }
  if ($workerEnv -notmatch [regex]::Escape('${env:supermini.build_flags}')) {
    Fail "[env:supermini-worker] must inherit \${env:supermini.build_flags} (identical to supermini plus the worker macro)."
  }
  if ($workerEnv -notmatch "-DPLANE_RADAR_ADSB_WORKER\s*=\s*1\b") {
    Fail "[env:supermini-worker] must define -DPLANE_RADAR_ADSB_WORKER=1."
  }
  Ok "Exactly one [env:supermini-worker] extends env:supermini and defines PLANE_RADAR_ADSB_WORKER=1."

  # The header must default the macro to 0 when undefined, so the default build is
  # worker-free with no extra flags and workerEnabled() tracks the macro.
  $workerH = Get-Indexed -Index $index -Rel $workerHRel
  if ($workerH.NoComments -notmatch "#\s*ifndef\s+PLANE_RADAR_ADSB_WORKER" -or
      $workerH.NoComments -notmatch "#\s*define\s+PLANE_RADAR_ADSB_WORKER\s+0") {
    Fail "adsb_worker.h must default PLANE_RADAR_ADSB_WORKER to 0 when undefined."
  }
  if ($workerH.Skeleton -notmatch "workerEnabled\s*\(\s*\)\s*\{\s*return\s+PLANE_RADAR_ADSB_WORKER\s*!=\s*0\s*;") {
    Fail "adsb_worker.h workerEnabled() must be a constexpr reflection of PLANE_RADAR_ADSB_WORKER != 0."
  }
  Ok "adsb_worker.h defaults the macro to 0 and pins workerEnabled() to it."

  # -----------------------------------------------------------------------
  # Invariant 2: compile-time gating; no worker state in the default build.
  # -----------------------------------------------------------------------
  Section "[2] Compile-time gating (default build has no worker state)"

  # The adapter's worker-OFF stub must carry no task/queue/stack state.
  $offForbidden = @(
    "xTaskCreateStatic", "xQueueCreateStatic", "\bxTaskCreate\b", "\bxQueueCreate\b",
    "StackType_t", "StaticTask_t", "StaticQueue_t", "s_worker_stack", "portMUX",
    "xQueueReceive", "xQueueSend", "\bworkerTask\b", "s_request_q", "s_result_q"
  )
  foreach ($p in $offForbidden) {
    if ($wReg.Off -match $p) {
      Fail "adsb_worker.cpp default (worker-off) stub must contain no worker task/queue/stack state; found '$p'."
    }
  }
  if ($wReg.On -notmatch "xTaskCreateStatic") {
    Fail "adsb_worker.cpp worker-enabled region is missing the static worker task creation."
  }
  Ok "adsb_worker.cpp heavy adapter is under #if PLANE_RADAR_ADSB_WORKER; the default stub is inert."

  # Worker-specific state / orchestration tokens must never appear in the code
  # compiled for the worker-OFF build (main.cpp + wifi_setup.cpp).
  $main = Get-Indexed -Index $index -Rel $mainRel
  $wifi = Get-Indexed -Index $index -Rel $wifiRel
  $mainReg = Get-WorkerRegions -Text $main.Skeleton
  $wifiReg = Get-WorkerRegions -Text $wifi.Skeleton

  $mainWorkerTokens = @(
    "\bg_worker_faulted\b", "\bhandleWorkerFault\b", "\bserviceAdsbAsync\b",
    "\bpumpWorkerResult\b", "\bworkerDispatch\b", "\bworkerTakeResult\b",
    "\bworkerBegin\b", "\bworkerStarted\b", "\bworkerRequestPause\b",
    "\bworkerQuiesced\b", "\bworkerResume\b", "\bworkerFaulted\b",
    "\bWorkerQuery\b", "\badsbWorkerPauseHook\b", "\badsbWorkerQuiescedHook\b",
    "\badsbWorkerResumeHook\b", "\bwifiSetNetworkWorkHooks\b", "\bWifiNetworkWorkHooks\b"
  )
  foreach ($t in $mainWorkerTokens) {
    if ($mainReg.Off -match $t) {
      Fail "main.cpp leaks worker orchestration into the default build: '$t' appears outside #if PLANE_RADAR_ADSB_WORKER."
    }
  }
  # wifi_setup.cpp: WifiNetworkWorkHooks / wifiSetNetworkWorkHooks are declared
  # unconditionally (inert in the default build), so only the truly worker-only
  # deferral tokens are required to be gated here.
  $wifiWorkerTokens = @(
    "NetworkWorkIntent", "\brequestConfigure\b", "\brequestErase\b",
    "\bconsumeIntent\b", "\bnetworkWorkIntentInit\b", "\bnwRequestPause\b",
    "\bnwQuiesced\b", "\bnwResume\b", "\bnwIntent\b", "\bnwPendingIntent\b",
    "\bs_nw_hooks\b", "\bs_nw_intent\b", "\bserviceDeferredNetworkWork\b"
  )
  foreach ($t in $wifiWorkerTokens) {
    if ($wifiReg.Off -match $t) {
      Fail "wifi_setup.cpp leaks worker deferral into the default build: '$t' appears outside #if PLANE_RADAR_ADSB_WORKER."
    }
  }
  Ok "All worker state / orchestration in main.cpp + wifi_setup.cpp is #if PLANE_RADAR_ADSB_WORKER-gated."

  # -----------------------------------------------------------------------
  # Invariant 3: static allocation only in the adapter + cores.
  # -----------------------------------------------------------------------
  Section "[3] Static allocation only (no dynamic task/queue/heap/STL)"

  $taskStatic = ([regex]::Matches($wReg.On, "xTaskCreateStatic\s*\(")).Count
  if ($taskStatic -ne 1) {
    Fail "the adapter must create EXACTLY one static task; found $taskStatic xTaskCreateStatic call(s)."
  }
  $queueStatic = ([regex]::Matches($wReg.On, "xQueueCreateStatic\s*\(")).Count
  if ($queueStatic -ne 2) {
    Fail "the adapter must create EXACTLY two static queues; found $queueStatic xQueueCreateStatic call(s)."
  }
  Ok "Adapter uses exactly one xTaskCreateStatic and two xQueueCreateStatic."

  # Dynamic APIs are matched PRECISELY so the *Static calls above are not false
  # positives (\bxTaskCreate\b never matches xTaskCreateStatic/PinnedToCore, and
  # \bxSemaphoreCreateMutex\b never matches xSemaphoreCreateMutexStatic). The ban
  # also covers the realistic heap-allocating FreeRTOS object creators (queues,
  # semaphores/mutexes, event groups, timers, stream/message buffers) that would
  # otherwise slip past the textual pvPortMalloc ban.
  $dynApis = @(
    @{ Name = "xTaskCreate (dynamic)";           Pattern = "\bxTaskCreate\b" },
    @{ Name = "xTaskCreatePinnedToCore";         Pattern = "\bxTaskCreatePinnedToCore\b" },
    @{ Name = "xQueueCreate (dynamic)";          Pattern = "\bxQueueCreate\b" },
    @{ Name = "xQueueCreateSet";                 Pattern = "\bxQueueCreateSet\b" },
    # Heap-allocating FreeRTOS creators whose *Static twins are the only allowed
    # form. Exact word boundaries so e.g. \bxSemaphoreCreateMutex\b never matches
    # xSemaphoreCreateMutexStatic (the trailing "Static" keeps the \b from firing).
    @{ Name = "xQueueGenericCreate (dynamic)";   Pattern = "\bxQueueGenericCreate\b" },
    @{ Name = "xSemaphoreCreateBinary";          Pattern = "\bxSemaphoreCreateBinary\b" },
    @{ Name = "xSemaphoreCreateCounting";        Pattern = "\bxSemaphoreCreateCounting\b" },
    @{ Name = "xSemaphoreCreateMutex";           Pattern = "\bxSemaphoreCreateMutex\b" },
    @{ Name = "xSemaphoreCreateRecursiveMutex";  Pattern = "\bxSemaphoreCreateRecursiveMutex\b" },
    @{ Name = "xEventGroupCreate (dynamic)";     Pattern = "\bxEventGroupCreate\b" },
    @{ Name = "xTimerCreate (dynamic)";          Pattern = "\bxTimerCreate\b" },
    @{ Name = "xStreamBufferCreate (dynamic)";   Pattern = "\bxStreamBufferCreate\b" },
    @{ Name = "xMessageBufferCreate (dynamic)";  Pattern = "\bxMessageBufferCreate\b" },
    @{ Name = "pvPortMalloc";                    Pattern = "\bpvPortMalloc\b" },
    @{ Name = "malloc";                          Pattern = "\bmalloc\b" },
    @{ Name = "calloc";                          Pattern = "\bcalloc\b" },
    @{ Name = "realloc";                         Pattern = "\brealloc\b" },
    @{ Name = "free";                            Pattern = "\bfree\b" },
    @{ Name = "operator new";                    Pattern = "\bnew\b" },
    @{ Name = "operator delete";                 Pattern = "\bdelete\b" },
    @{ Name = "std::vector";                     Pattern = "std::vector\b" },
    @{ Name = "std::queue";                      Pattern = "std::queue\b" },
    @{ Name = "std::deque";                      Pattern = "std::deque\b" },
    @{ Name = "std::list";                       Pattern = "std::list\b" },
    @{ Name = "std::map";                        Pattern = "std::map\b" },
    @{ Name = "std::unordered_*";                Pattern = "std::unordered_" },
    @{ Name = "std::function";                   Pattern = "std::function\b" }
  )
  # Scope: the adapter's worker-enabled code + the pure cores + their headers.
  $coreSet = @(
    @{ Rel = $workerRel;  Text = $wReg.On },
    @{ Rel = $workerHRel; Text = (Get-Indexed -Index $index -Rel $workerHRel).Skeleton },
    @{ Rel = $protoCRel;  Text = (Get-Indexed -Index $index -Rel $protoCRel).Skeleton },
    @{ Rel = $protoHRel;  Text = (Get-Indexed -Index $index -Rel $protoHRel).Skeleton },
    @{ Rel = $intentCRel; Text = (Get-Indexed -Index $index -Rel $intentCRel).Skeleton },
    @{ Rel = $intentHRel; Text = (Get-Indexed -Index $index -Rel $intentHRel).Skeleton }
  )
  foreach ($f in $coreSet) {
    foreach ($api in $dynApis) {
      if ($f.Text -match $api.Pattern) {
        Fail "dynamic allocation '$($api.Name)' is forbidden in worker/core file $($f.Rel)."
      }
    }
  }
  Ok "No dynamic task/queue/heap/STL or heap-allocating FreeRTOS object creators in the worker adapter or the pure cores."

  # -----------------------------------------------------------------------
  # Invariant 4: fixed stack/queue constants; no task teardown.
  # -----------------------------------------------------------------------
  Section "[4] Fixed stack/queue sizing; no task deletion/recreation"

  $on = $wReg.On
  if ($on -notmatch "kWorkerStackBytes\s*=\s*8192\b") {
    Fail "the worker stack must be pinned at 8192 bytes (kWorkerStackBytes = 8192)."
  }
  if ($on -notmatch "alignas\s*\(\s*16\s*\)\s*StackType_t\s+s_worker_stack\s*\[\s*kWorkerStackBytes\s*\]") {
    Fail "the worker stack must be 'alignas(16) StackType_t s_worker_stack[kWorkerStackBytes]'."
  }
  if ($on -notmatch "static_assert\s*\(\s*sizeof\s*\(\s*StackType_t\s*\)\s*==\s*1") {
    Fail "the adapter must static_assert sizeof(StackType_t) == 1 (stack depth is expressed in bytes)."
  }
  if ($on -notmatch "kWorkerPriority\s*=\s*1\b") {
    Fail "the worker task priority must be pinned at 1 (kWorkerPriority = 1)."
  }
  if ($on -notmatch "kRequestQueueLen\s*=\s*1\b") {
    Fail "the request queue depth must be pinned at 1 (kRequestQueueLen = 1)."
  }
  if ($on -notmatch "kResultQueueLen\s*=\s*1\b") {
    Fail "the result queue depth must be pinned at 1 (kResultQueueLen = 1)."
  }
  foreach ($buf in @(
      @{ Name = "static TCB";            Pattern = "StaticTask_t\s+s_worker_tcb\s*;" },
      @{ Name = "static request queue";  Pattern = "StaticQueue_t\s+s_request_queue_buf\s*;" },
      @{ Name = "static result queue";   Pattern = "StaticQueue_t\s+s_result_queue_buf\s*;" },
      @{ Name = "request backing store"; Pattern = "s_request_storage\s*\[" },
      @{ Name = "result backing store";  Pattern = "s_result_storage\s*\[" })) {
    if ($on -notmatch $buf.Pattern) {
      Fail "the adapter must declare the $($buf.Name) as static backing storage."
    }
  }
  # The one static creation call must consume the pinned stack/priority/buffers.
  # This is a CALL, so balance its parentheses (not braces) to grab the args.
  $createBody = ""
  $ci = $on.IndexOf("xTaskCreateStatic(")
  if ($ci -ge 0) {
    $pOpen = $on.IndexOf('(', $ci)
    $pDepth = 0
    for ($j = $pOpen; $j -lt $on.Length; $j++) {
      $pch = $on[$j]
      if ($pch -eq '(') { $pDepth++ }
      elseif ($pch -eq ')') { $pDepth--; if ($pDepth -eq 0) { $createBody = $on.Substring($pOpen, $j - $pOpen + 1); break } }
    }
  }
  if ([string]::IsNullOrEmpty($createBody)) { Fail "could not parse the xTaskCreateStatic() argument list." }
  foreach ($need in @("kWorkerStackBytes", "kWorkerPriority", "s_worker_stack", "s_worker_tcb")) {
    if ($createBody -notmatch [regex]::Escape($need)) {
      Fail "xTaskCreateStatic must pass $need (pinned stack depth / priority / static stack + TCB)."
    }
  }
  $teardown = @(
    @{ Name = "vTaskDelete";     Pattern = "\bvTaskDelete\b" },
    @{ Name = "vTaskSuspend";    Pattern = "\bvTaskSuspend\b" },
    @{ Name = "vTaskResume";     Pattern = "\bvTaskResume\b" },
    @{ Name = "xTaskAbortDelay"; Pattern = "\bxTaskAbortDelay\b" }
  )
  foreach ($td in $teardown) {
    if ($worker.Skeleton -match $td.Pattern) {
      Fail "the long-lived worker task must never be torn down/recreated; found '$($td.Name)'."
    }
  }
  Ok "Stack 8192/alignas(16)/sizeof==1, priority 1, queue depths 1, static buffers, no task teardown."

  # -----------------------------------------------------------------------
  # Invariant 5: queue payloads are trivially-copyable metadata only.
  # -----------------------------------------------------------------------
  Section "[5] Queue payloads carry metadata only (trivially copyable)"

  $payloadForbidden = @(
    @{ Name = "AircraftSnapshot";     Pattern = "\bAircraftSnapshot\b" },
    @{ Name = "framebuffer sprite";   Pattern = "\bLGFX_Sprite\b|\bSprite\b" },
    @{ Name = "String";               Pattern = "\bString\b" },
    @{ Name = "std::string";          Pattern = "std::string\b" },
    @{ Name = "Wi-Fi credential/config"; Pattern = "wifi_config|StaConfig|WifiCred|Credential|Preferences" },
    @{ Name = "raw pointer field";    Pattern = "[A-Za-z_][\w:<>]*\s*\*\s*\w+\s*;" }
  )
  $reqBody = Get-FunctionBody -Skeleton $worker.Skeleton -SignaturePattern "struct\s+WorkerRequest\b"
  if ($null -eq $reqBody) { Fail "could not locate the WorkerRequest payload struct in adsb_worker.cpp." }
  $resBody = Get-FunctionBody -Skeleton $worker.Skeleton -SignaturePattern "struct\s+WorkerResultMsg\b"
  if ($null -eq $resBody) { Fail "could not locate the WorkerResultMsg payload struct in adsb_worker.cpp." }

  foreach ($pl in @(@{ Name = "WorkerRequest"; Body = $reqBody }, @{ Name = "WorkerResultMsg"; Body = $resBody })) {
    foreach ($f in $payloadForbidden) {
      if ($pl.Body -match $f.Pattern) {
        Fail "queue payload $($pl.Name) must not embed '$($f.Name)' (metadata-only, trivially copyable)."
      }
    }
  }
  foreach ($need in @("generation", "settings_revision", "connectivity_epoch")) {
    if ($reqBody -notmatch "\b$need\b") { Fail "WorkerRequest must carry '$need'." }
    if ($resBody -notmatch "\b$need\b") { Fail "WorkerResultMsg must carry '$need'." }
  }
  if ($resBody -notmatch "\bCandidateResult\b") { Fail "WorkerResultMsg must carry the CandidateResult (handle, not a snapshot)." }
  if ($resBody -notmatch "\bworker_cancelled\b") { Fail "WorkerResultMsg must carry the explicit cooperative-cancel flag (worker_cancelled)." }
  foreach ($t in @("WorkerRequest", "WorkerResultMsg")) {
    if ($worker.Skeleton -notmatch "static_assert\s*\(\s*std::is_trivially_copyable<\s*$t\s*>::value") {
      Fail "the adapter must static_assert std::is_trivially_copyable<$t> for the byte-copied queue."
    }
  }
  Ok "WorkerRequest/WorkerResultMsg are trivially-copyable metadata (gen/rev/epoch/CandidateResult/cancel; no snapshot/sprite/cred/String/pointer)."

  # -----------------------------------------------------------------------
  # Invariant 6: ownership boundaries; single framebuffer + two snapshots.
  # -----------------------------------------------------------------------
  Section "[6] Ownership boundaries; one framebuffer, two snapshots"

  if ($wReg.On -match "\bpublishCandidate\b") {
    Fail "the worker adapter must NEVER publish; publishCandidate() belongs to the main task only."
  }
  foreach ($need in @("fetchCandidateControlled", "discardCandidate")) {
    if ($wReg.On -notmatch "\b$need\b") {
      Fail "the worker adapter must use $need (fetch via the controlled seam; discard only on fail-closed paths)."
    }
  }
  if ($main.Skeleton -notmatch "\bpublishCandidate\b") {
    Fail "main.cpp must own publication (publishCandidate) of the worker's candidate."
  }
  if ($main.Skeleton -notmatch "\bworkerTakeResult\b") {
    Fail "main.cpp must pump/take worker results (workerTakeResult)."
  }
  Ok "Adapter fetches (controlled) + discards on fail-closed paths and never publishes; main pumps + publishes."

  # Reuse the provisioning-gate single-framebuffer pattern: exactly one full-frame
  # sprite and one createSprite; and exactly the store's two AircraftSnapshots.
  $spriteDecls = @(Find-Across -Index $index -Pattern "\bLGFX_Sprite\s+\w+" -On "Skeleton")
  if ($spriteDecls.Count -ne 1) {
    Fail "expected EXACTLY one full-frame LGFX_Sprite; found $($spriteDecls.Count): $((Rels $spriteDecls) -join ', ')."
  }
  if ($spriteDecls[0].Rel -ne $displayRel) {
    Fail "the single LGFX_Sprite must live in $displayRel, not $($spriteDecls[0].Rel)."
  }
  $createSprite = @(Find-Across -Index $index -Pattern "\.createSprite\s*\(" -On "Skeleton")
  if ($createSprite.Count -ne 1) {
    Fail "expected EXACTLY one createSprite() call; found $($createSprite.Count): $((Rels $createSprite) -join ', ')."
  }
  $snapArrays = @(Find-Across -Index $index -Pattern "\bAircraftSnapshot\s+\w+\s*\[\s*2\s*\]" -On "Skeleton")
  if ($snapArrays.Count -ne 1 -or $snapArrays[0].Rel -ne $storeRel) {
    Fail "expected EXACTLY one two-slot 'AircraftSnapshot ...[2]' (the SnapshotStore); found $($snapArrays.Count): $((Rels $snapArrays) -join ', ')."
  }
  $snapStorage = @(Find-Across -Index $index -Pattern "\bAircraftSnapshot\s+\w+" -On "Skeleton")
  if ($snapStorage.Count -ne 1) {
    Fail "a third AircraftSnapshot was introduced (only the store's two-slot array may exist); found in: $((Rels $snapStorage) -join ', ')."
  }
  Ok "Exactly one framebuffer sprite + one createSprite, and exactly the store's two AircraftSnapshots."

  # -----------------------------------------------------------------------
  # Invariant 7: worker isolation (no Wi-Fi/UI/NVS/publish/transport-stop).
  # -----------------------------------------------------------------------
  Section "[7] Worker isolation (adsb_worker.cpp)"

  $isolation = @(
    @{ Name = "wifiLoop";                 Pattern = "\bwifiLoop\b" },
    @{ Name = "WiFi API";                 Pattern = "\bWiFi\b" },
    @{ Name = "WiFiClientSecure";         Pattern = "WiFiClientSecure" },
    @{ Name = "esp_wifi_*";               Pattern = "\besp_wifi" },
    @{ Name = "cross-task .stop()";       Pattern = "\.stop\s*\(" },
    @{ Name = "UI namespace";             Pattern = "\bui::" },
    @{ Name = "LGFX/sprite";              Pattern = "\bLGFX|\bLGFX_Sprite\b" },
    @{ Name = "display/status draw";      Pattern = "drawScreen|updateDisplay|radarDisplay|RadarDisplay|\btft\b" },
    @{ Name = "portal/config";            Pattern = "services::portal::|portalSessionUpdate|config_portal|\bPortalInput\b" },
    @{ Name = "settings mutation";        Pattern = "services::settings::" },
    @{ Name = "NVS/Preferences";          Pattern = "\bPreferences\b|\bnvs_|\bNVS\b" },
    @{ Name = "SNTP";                     Pattern = "\bsntp\b|configTime|configTzTime|\bSNTP\b" },
    @{ Name = "cert-floor ratchet";       Pattern = "noteVerifiedCertFloor" }
  )
  foreach ($iso in $isolation) {
    if ($worker.Skeleton -match $iso.Pattern) {
      Fail "the worker task must stay isolated; forbidden '$($iso.Name)' found in adsb_worker.cpp."
    }
  }
  # The cancellation callback must remain a pure protocol query -- never a
  # transport/Wi-Fi mutation.
  $cancelBody = Get-FunctionBody -Skeleton $worker.Skeleton -SignaturePattern "bool\s+workerCancel\s*\("
  if ($null -eq $cancelBody) { Fail "could not locate workerCancel() in adsb_worker.cpp." }
  if ($cancelBody -notmatch "workerCancelRequested") {
    Fail "workerCancel() must resolve cancellation via core::workerCancelRequested (a protocol query)."
  }
  foreach ($bad in @("\bstop\b", "\bWiFi\b", "\besp_wifi", "disconnect", "discardCandidate", "publishCandidate")) {
    if ($cancelBody -match $bad) {
      Fail "workerCancel() must be a pure protocol query; found transport/side-effect token matching '$bad'."
    }
  }
  Ok "adsb_worker.cpp touches no Wi-Fi/UI/portal/settings/NVS/SNTP/cert-floor/transport-stop; cancel is a pure query."

  # -----------------------------------------------------------------------
  # Invariant 8: main runtime contracts.
  # -----------------------------------------------------------------------
  Section "[8] Main runtime contracts (pumping, quiesce, fault)"

  $loopBody = Get-FunctionBody -Skeleton $main.Skeleton -SignaturePattern "void\s+loop\s*\(\s*\)"
  if ($null -eq $loopBody) { Fail "could not locate loop() in main.cpp." }
  $wifiLoopCount = ([regex]::Matches($loopBody, "\bwifiLoop\s*\(\s*\)")).Count
  if ($wifiLoopCount -ne 1) {
    Fail "loop() must call the top-level wifiLoop() exactly once; found $wifiLoopCount."
  }
  $loopCode = Get-CodeOnly -Text $loopBody
  if ($loopCode -notmatch "pumpWorkerResult\s*\(\s*\)\s*;\s*wifiLoop\s*\(\s*\)\s*;") {
    Fail "loop() must pump worker results IMMEDIATELY BEFORE the top-level wifiLoop()."
  }
  if ($loopCode -notmatch "wifiLoop\s*\(\s*\)\s*;\s*pumpWorkerResult\s*\(\s*\)\s*;") {
    Fail "loop() must pump worker results IMMEDIATELY AFTER the top-level wifiLoop()."
  }
  foreach ($ownSig in @("if\s*\(\s*owns\s*\)", "if\s*\(\s*wifiOwnsDisplay\s*\(\s*\)\s*\)")) {
    $ownBlock = Get-FunctionBody -Skeleton $loopBody -SignaturePattern $ownSig
    if ($null -eq $ownBlock) { Fail "could not locate a display-ownership return block ($ownSig) in loop()." }
    if ($ownBlock -notmatch "\breturn\b") { Fail "the display-ownership block ($ownSig) must early-return." }
    if ($ownBlock -notmatch "pumpWorkerResult\s*\(") {
      Fail "the display-ownership return path ($ownSig) must keep pumping worker results (deferred Configure/Erase can quiesce there)."
    }
  }
  Ok "loop() pumps results before + after the top-level wifiLoop() and on both display-ownership return paths."

  $pumpBody = Get-FunctionBody -Skeleton $main.Skeleton -SignaturePattern "void\s+pumpWorkerResult\s*\(\s*\)"
  if ($null -eq $pumpBody) { Fail "could not locate pumpWorkerResult() in main.cpp." }
  if ($pumpBody -notmatch "workerPollActionForNoResult" -or $pumpBody -notmatch "\bworkerFaulted\b") {
    Fail "pumpWorkerResult() must observe workerFaulted() on the empty-queue path (result-less faults)."
  }
  $discardCase = Get-CaseRegion -Body $pumpBody -LabelPattern "DiscardAndAbort\s*:"
  if ($null -eq $discardCase) { Fail "could not locate the Quiesced/DiscardAndAbort case in pumpWorkerResult()." }
  if ($discardCase -notmatch "\bdiscardCandidate\b" -or $discardCase -notmatch "adsbFetchAbortedForPause") {
    Fail "the Quiesced path must discard the candidate and call adsbFetchAbortedForPause()."
  }
  foreach ($bad in @("finishAdsbFetch", "adsbFetchCompleted", "publishCandidate")) {
    if ($discardCase -match $bad) {
      Fail "the Quiesced path must NOT publish/complete; found '$bad' (it must map to discard + abort-for-pause only)."
    }
  }
  $publishCase = Get-CaseRegion -Body $pumpBody -LabelPattern "::Publish\s*:"
  if ($null -eq $publishCase -or $publishCase -notmatch "finishAdsbFetch") {
    Fail "the Completed/Publish path must publish via the shared finishAdsbFetch()."
  }
  Ok "Quiesced -> discard + adsbFetchAbortedForPause (never Completed/publish); empty-queue path observes workerFaulted()."

  $asyncBody = Get-FunctionBody -Skeleton $main.Skeleton -SignaturePattern "void\s+serviceAdsbAsync\s*\(\s*\)"
  if ($null -eq $asyncBody) { Fail "could not locate serviceAdsbAsync() in main.cpp." }
  if ($asyncBody -notmatch "if\s*\(\s*g_worker_faulted\s*\)\s*\{\s*return") {
    Fail "serviceAdsbAsync() must halt dispatch while faulted (if (g_worker_faulted) return)."
  }
  if ($asyncBody -match "\bserviceAdsbSync\b") {
    Fail "serviceAdsbAsync() must never fall back to the synchronous fetch while the worker task exists."
  }
  $faultBody = Get-FunctionBody -Skeleton $main.Skeleton -SignaturePattern "void\s+handleWorkerFault\s*\(\s*\)"
  if ($null -eq $faultBody) { Fail "could not locate handleWorkerFault() in main.cpp." }
  if ($faultBody -notmatch "g_worker_faulted\s*=\s*true" -or $faultBody -notmatch "adsbFetchAbortedForPause") {
    Fail "handleWorkerFault() must latch g_worker_faulted and unwind the in-flight poll (adsbFetchAbortedForPause)."
  }
  # The sync fallback must be reachable ONLY when the worker never started.
  if ($loopBody -notmatch "if\s*\(\s*services::adsb::workerStarted\s*\(\s*\)\s*\)") {
    Fail "loop() must dispatch async only when workerStarted(); sync is the not-started fallback."
  }
  Ok "A worker fault halts dispatch with no synchronous fallback while the long-lived task exists."

  # -----------------------------------------------------------------------
  # Invariant 9: provisioning (Configure/Erase) contracts.
  # -----------------------------------------------------------------------
  Section "[9] Deferred Configure/Erase contracts (wifi_setup.cpp)"

  $btnBody = Get-FunctionBody -Skeleton $wifi.Skeleton -SignaturePattern "void\s+handleButtonEvent\s*\("
  if ($null -eq $btnBody) { Fail "could not locate handleButtonEvent() in wifi_setup.cpp." }
  $btnReg = Get-WorkerRegions -Text $btnBody
  foreach ($need in @("requestConfigure", "requestErase", "nwRequestPause")) {
    if ($btnReg.On -notmatch "\b$need\b") {
      Fail "the worker Configure/Erase button path must latch $need before any radio/NVS work."
    }
  }
  foreach ($bad in @("feed\s*\(\s*PortalInput::ConfigureButton", "feed\s*\(\s*PortalInput::EraseConfirmed")) {
    if ($btnReg.On -match $bad) {
      Fail "the worker button path must DEFER (latch + pause), not feed the portal FSM immediately; found '$bad'."
    }
  }
  Ok "Worker Configure/Erase buttons latch requestConfigure/requestErase + pause (no immediate feed)."

  $deferBody = Get-FunctionBody -Skeleton $wifi.Skeleton -SignaturePattern "void\s+serviceDeferredNetworkWork\s*\("
  if ($null -eq $deferBody) { Fail "could not locate serviceDeferredNetworkWork() in wifi_setup.cpp." }
  $mGuard = [regex]::Match($deferBody, "if\s*\(\s*!\s*nwQuiesced\s*\(\s*\)\s*\)")
  $mConsume = [regex]::Match($deferBody, "consumeIntent\s*\(")
  if (-not $mGuard.Success -or -not $mConsume.Success -or $mGuard.Index -ge $mConsume.Index) {
    Fail "serviceDeferredNetworkWork() must gate consumeIntent() behind a proven-quiescence guard (if (!nwQuiesced()) return)."
  }
  # The guard must be a braced block that actually bails out (return) BEFORE the
  # intent is consumed -- reject code that keeps the 'if (!nwQuiesced())' text but
  # deletes its return, which would fall through and consume the intent while the
  # worker is still live.
  $afterGuard = $deferBody.Substring($mGuard.Index + $mGuard.Length)
  if ($afterGuard -notmatch '^\s*\{') {
    Fail "serviceDeferredNetworkWork()'s 'if (!nwQuiesced())' guard must be a braced { ... } block that returns before consumeIntent()."
  }
  $guardBlock = Get-FunctionBody -Skeleton $deferBody -SignaturePattern "if\s*\(\s*!\s*nwQuiesced\s*\(\s*\)\s*\)"
  if ($null -eq $guardBlock) {
    Fail "serviceDeferredNetworkWork()'s 'if (!nwQuiesced())' guard block braces are unbalanced or missing."
  }
  if ($guardBlock -notmatch "\breturn\b") {
    Fail "serviceDeferredNetworkWork()'s 'if (!nwQuiesced())' guard must actually 'return' (bail out) when the worker is not quiesced, before consumeIntent()."
  }
  $guardEnd = $mGuard.Index + $mGuard.Length + $afterGuard.IndexOf('{') + $guardBlock.Length
  if ($guardEnd -gt $mConsume.Index) {
    Fail "serviceDeferredNetworkWork() must close its proven-quiescence guard before consuming the intent (consumeIntent must follow the guard block)."
  }
  if ($deferBody -notmatch "consumeIntent\s*\(\s*nwIntent\s*\(\s*\)\s*,\s*true\s*\)") {
    Fail "serviceDeferredNetworkWork() must consume the intent only once quiescence is proven (consumeIntent(nwIntent(), true))."
  }
  Ok "Deferred service's quiescence guard returns before consuming the intent, only after proven worker quiescence."

  # Erase supersedes Configure in the pure core.
  $intentC = Get-Indexed -Index $index -Rel $intentCRel
  $reqCfgBody = Get-FunctionBody -Skeleton $intentC.Skeleton -SignaturePattern "void\s+requestConfigure\s*\("
  $reqEraseBody = Get-FunctionBody -Skeleton $intentC.Skeleton -SignaturePattern "void\s+requestErase\s*\("
  if ($null -eq $reqCfgBody -or $null -eq $reqEraseBody) { Fail "could not locate requestConfigure/requestErase in network_work_intent.cpp." }
  if ($reqCfgBody -notmatch "pending\s*==\s*(core::)?NetworkWorkIntent::Erase" -or $reqCfgBody -notmatch "\breturn\b") {
    Fail "requestConfigure() must NOT override a pending Erase (Erase strictly supersedes Configure)."
  }
  if ($reqEraseBody -notmatch "pending\s*=\s*(core::)?NetworkWorkIntent::Erase\s*;") {
    Fail "requestErase() must latch Erase (superseding any pending Configure)."
  }
  Ok "Erase supersedes Configure via the pure core."

  # pause_adsb precedes same-action radio changes; resume reuses the existing latch.
  $applyBody = Get-FunctionBody -Skeleton $wifi.Skeleton -SignaturePattern "void\s+applyActions\s*\("
  if ($null -eq $applyBody) { Fail "could not locate applyActions() in wifi_setup.cpp." }
  $iPause = [regex]::Match($applyBody, "a\.pause_adsb")
  $iDisc = [regex]::Match($applyBody, "a\.disconnect_sta")
  $iAp = [regex]::Match($applyBody, "a\.start_ap_radio")
  if (-not $iPause.Success -or -not $iDisc.Success -or -not $iAp.Success) {
    Fail "applyActions() must handle pause_adsb, disconnect_sta and start_ap_radio."
  }
  if ($iPause.Index -ge $iDisc.Index -or $iPause.Index -ge $iAp.Index) {
    Fail "the pause_adsb hook must run BEFORE the same-action radio changes (disconnect_sta / start_ap_radio)."
  }
  $resumeBlock = Get-FunctionBody -Skeleton $applyBody -SignaturePattern "if\s*\(\s*a\.resume_adsb\s*\)"
  if ($null -eq $resumeBlock -or $resumeBlock -notmatch "\bnwResume\b") {
    Fail "applyActions() must resume the worker on the resume_adsb edge (nwResume)."
  }
  if ($resumeBlock -match "s_immediate_fetch_pending" -or $resumeBlock -match "adsbSettingsChanged") {
    Fail "the resume path must reuse the EXISTING force_immediate_adsb_fetch latch, not introduce a second immediate latch."
  }
  if ($applyBody -notmatch "a\.force_immediate_adsb_fetch" -or $applyBody -notmatch "s_immediate_fetch_pending\s*=\s*true") {
    Fail "applyActions() must drive the one post-resume refresh through the existing force_immediate_adsb_fetch latch."
  }
  Ok "pause_adsb precedes radio changes; resume reuses the existing immediate-fetch latch."

  # -----------------------------------------------------------------------
  # Invariant 10: no production leakage of the native test-access seam.
  # -----------------------------------------------------------------------
  Section "[10] No PLANE_RADAR_NATIVE_TEST_ACCESS / test-support worker seam"

  if ($workerEnv -match "PLANE_RADAR_NATIVE_TEST_ACCESS") {
    Fail "[env:supermini-worker] must never define PLANE_RADAR_NATIVE_TEST_ACCESS."
  }
  foreach ($f in $coreSet) {
    if ($f.Text -match "PLANE_RADAR_NATIVE_TEST_ACCESS") {
      Fail "worker/core file $($f.Rel) must not reference the native test-access seam."
    }
    if ($f.Text -match "test/support|test\\support") {
      Fail "worker/core file $($f.Rel) must not include a test-support seam."
    }
  }
  Ok "No worker test-access seam leaks into the firmware."

  if (-not $Quiet) { Write-Host "All ADS-B worker policy invariants verified." -ForegroundColor Green }
}

# ===========================================================================
# Negative tamper self-test.
# ===========================================================================
function Invoke-TamperSelfTest {
  param([Parameter(Mandatory)][string]$Root)

  Write-Host "=== Negative tamper self-test (proves the gate REJECTS regressions) ===" -ForegroundColor Cyan

  $tempRoot = Join-Path $Root (".adsbgate-selftest-" + [guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
  try {
    foreach ($sub in @("src", "include")) {
      $srcDir = Join-Path $Root $sub
      if (Test-Path -LiteralPath $srcDir) {
        Copy-Item -LiteralPath $srcDir -Destination (Join-Path $tempRoot $sub) -Recurse -Force
      }
    }
    Copy-Item -LiteralPath (Join-Path $Root "platformio.ini") -Destination (Join-Path $tempRoot "platformio.ini") -Force

    Invoke-WorkerPolicyGate -Root $tempRoot -Quiet
    Write-Host "  OK (baseline): the untampered mirror passes the gate." -ForegroundColor Green

    $cases = @(
      @{ Name = "1. enable worker in default [env:supermini]"; Rel = "platformio.ini";
        Mutate = { param($t) $t -replace '(-DARDUINO_USB_CDC_ON_BOOT=1)', "`$1`n  -DPLANE_RADAR_ADSB_WORKER=1" } },
      @{ Name = "2. dynamic xTaskCreate replaces xTaskCreateStatic"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace 's_task = xTaskCreateStatic\(', 's_task = xTaskCreate(' } },
      @{ Name = "3. dynamic xQueueCreate replaces one xQueueCreateStatic"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace 's_request_q = xQueueCreateStatic\(', 's_request_q = xQueueCreate(' } },
      @{ Name = "4. dynamic std::vector added to worker code"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(void workerTask\(void\* /\*param\*/\) \{)', "`$1`n  std::vector<int> tamper_leak;" } },
      @{ Name = "5. worker stack changed away from 8192"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace 'kWorkerStackBytes = 8192;', 'kWorkerStackBytes = 16384;' } },
      @{ Name = "6. request queue depth changed from 1"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace 'kRequestQueueLen = 1;', 'kRequestQueueLen = 2;' } },
      @{ Name = "7. wifiLoop() added to the worker cancel path"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(bool workerCancel\(void\* ctx\) \{)', "`$1`n  wifiLoop();" } },
      @{ Name = "8. publishCandidate() added to worker code"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(msg\.candidate = candidate;)', "`$1`n    publishCandidate(candidate.handle, 0);" } },
      @{ Name = "9. AircraftSnapshot added to a queue payload (third snapshot)"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(struct WorkerResultMsg \{)', "`$1`n  AircraftSnapshot snap;" } },
      @{ Name = "10. empty-queue workerFaulted() observation removed"; Rel = "src\main.cpp";
        Mutate = { param($t) $t -replace 'services::adsb::workerFaulted\(\)', 'false' } },
      @{ Name = "11. Quiesced path publishes instead of discard/abort"; Rel = "src\main.cpp";
        Mutate = { param($t) $t -replace '(services::adsb::discardCandidate\(result\.candidate\.handle\);\s*)core::adsbFetchAbortedForPause\(&g_poll\);', "`$1finishAdsbFetch(result.candidate, result.connectivity_epoch);" } },
      @{ Name = "12. worker Erase branch feeds the FSM immediately (bypasses quiescence)"; Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace '(core::requestErase\(nwIntent\(\)\);)', "`$1`n      feed(PortalInput::EraseConfirmed, now_ms);" } },
      @{ Name = "13. required pump before top-level wifiLoop removed"; Rel = "src\main.cpp";
        Mutate = { param($t) ([regex]'pumpWorkerResult\(\)\s*;').Replace($t, '(void)0;', 1) } },
      @{ Name = "14. dynamic FreeRTOS mutex creator added to worker code"; Rel = "src\services\adsb_worker.cpp";
        Mutate = { param($t) $t -replace '(void workerTask\(void\* /\*param\*/\) \{)', "`$1`n  xSemaphoreCreateMutex();" } },
      @{ Name = "15. quiescence guard keeps its text but loses its return"; Rel = "src\services\wifi_setup.cpp";
        Mutate = { param($t) $t -replace 'return;  // worker not yet Paused', '// worker not yet Paused' } }
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
          Invoke-WorkerPolicyGate -Root $tempRoot -Quiet
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

    Invoke-WorkerPolicyGate -Root $tempRoot -Quiet
    Write-Host "  OK (post-restore): the mirror passes again after all mutations reverted." -ForegroundColor Green
    Write-Host "Tamper self-test passed: the gate rejects every representative regression (15 tamper cases)." -ForegroundColor Green
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
  Write-Host "=== Phase 9 ADS-B worker policy gate ($ProjectRoot) ===" -ForegroundColor Cyan
  Invoke-WorkerPolicyGate -Root $ProjectRoot
}
