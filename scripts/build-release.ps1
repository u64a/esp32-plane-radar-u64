<#
.SYNOPSIS
  Phase 12 fail-closed LOCAL release builder for the ESP32-C3 Plane Radar firmware.

.DESCRIPTION
  Produces a reproducible, self-describing, re-verifiable release package for all
  five firmware environments under release/<full-git-sha>/, with persistent
  current-head ELF/binary proofs, a manifest, and a checksum manifest.

  Fail-closed preconditions (any failure aborts BEFORE building or publishing):
    * PlatformIO Core is EXACTLY the pinned 6.1.19.
    * git HEAD exists; the tracked/index worktree is clean; there are NO untracked
      files; and NO git remote is configured (repo is intentionally local-only).
    * The resolved output path lives strictly inside <repo>/release/ and its leaf
      is exactly the 40-hex HEAD commit (no path traversal / broad deletion).
    * Every required source gate is green (scripts/native-test.ps1 -> 606/41 plus
      check-native-test-access, CA, provisioning, worker, and diagnostics gates).

  Then it deletes .pio ONCE, freshly builds+merges exactly the five envs, enforces
  the approved EXACT resource/file-size policy, runs the binary proof engine
  (nm symbol evidence + binary-safe byte scan), and only if every invariant passes
  does it stage and publish the package. Refuses to overwrite an existing release
  child unless -Force is given (which replaces ONLY that exact validated path).

  This package binds the EXACT current-head binaries only; it does NOT claim raw
  byte equivalence to any prior phase's artifacts.

.PARAMETER Force
  Replace an already-published release/<sha>/ directory. Only that exact validated
  child path is removed; a broad/unresolved path is never deleted.

.PARAMETER SkipSourceGates
  DIAGNOSTIC ONLY. Skips the required source gates and records gate status
  "skipped" in the manifest, producing a NON-CERTIFIED package. Never use for a
  real release.

.PARAMETER SkipBuild
  DIAGNOSTIC ONLY. Reuses existing .pio/build/<env> artifacts instead of deleting
  .pio and rebuilding. Records build.clean_build=false. Never use for a real
  release.

.PARAMETER AllowDirtyWorktree
  DIAGNOSTIC ONLY. Skips the clean-worktree / no-untracked-files preconditions
  (the no-remote precondition is ALWAYS enforced). Records build.certified=false.
  Never use for a real release.

.PARAMETER OutputRoot
  Optional override for the release root (default: <repo>/release). Must still
  resolve strictly inside the repository.
#>
[CmdletBinding()]
param(
  [switch]$Force,
  [switch]$SkipSourceGates,
  [switch]$SkipBuild,
  [switch]$AllowDirtyWorktree,
  [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
# .NET file APIs resolve relative paths against [Environment]::CurrentDirectory,
# NOT PowerShell's $PWD. Pin both to the repo root so every helper is safe.
Set-Location -LiteralPath $repoRoot
[Environment]::CurrentDirectory = $repoRoot
. "$PSScriptRoot\release-common.ps1"

function Write-Step([string]$Message) { Write-Host "`n=== $Message ===" -ForegroundColor Cyan }
function Write-Ok([string]$Message)   { Write-Host "  OK: $Message" }

# ===========================================================================
# 0. Load the tracked release policy (single source of truth).
# ===========================================================================
$policyPath = Join-Path $PSScriptRoot 'release-policy.json'
$policy = Read-JsonFile $policyPath
$envNames = @($policy.environments | ForEach-Object { $_.name })

# ===========================================================================
# 1. PlatformIO must be EXACTLY the pinned version.
# ===========================================================================
Write-Step "Preconditions"
$pio = Resolve-PlatformIoExe
$pioVersion = (& $pio --version 2>&1 | Select-Object -First 1)
$expectedPio = "PlatformIO Core, version $($policy.pins.platformio_core)"
if ($LASTEXITCODE -ne 0 -or "$pioVersion".Trim() -ne $expectedPio) {
  throw "PlatformIO $($policy.pins.platformio_core) is required; found: $pioVersion"
}
Write-Ok "PlatformIO $($policy.pins.platformio_core)"

# ===========================================================================
# 2. Git: HEAD exists, clean tracked+index, no untracked, no remote.
# ===========================================================================
$git = Get-RepoGitState $repoRoot
if ($git.HasRemote) {
  throw "A git remote is configured ($($git.RemoteCount)); this repository must stay local-only. Aborting."
}
if ($AllowDirtyWorktree) {
  Write-Warning "AllowDirtyWorktree: skipping clean-worktree / no-untracked checks (NON-CERTIFIED build)."
} else {
  if ($git.TrackedDirty) {
    throw "The tracked/index worktree is not clean:`n$($git.TrackedLines -join "`n")"
  }
  if ($git.UntrackedCount -gt 0) {
    throw "Untracked files are present (release must build from a pristine worktree):`n$($git.UntrackedLines -join "`n")"
  }
}
Write-Ok "git HEAD $($git.ShortCommit) on '$($git.Branch)': no remote"

# ===========================================================================
# 3. Resolve + validate the publish target (strictly inside release/, 40-hex leaf).
# ===========================================================================
$releaseRootRaw = $OutputRoot
if (-not $releaseRootRaw) { $releaseRootRaw = Join-Path $repoRoot $policy.local_only.release_root_relative }
$releaseRoot = Get-FullPathSafe $releaseRootRaw
if (-not (Test-PathInside -Base $repoRoot -Candidate $releaseRoot -AllowEqual)) {
  throw "Release root '$releaseRoot' is not inside the repository."
}
$sha = $git.Commit
if (-not (Test-Sha1Hex $sha)) { throw "HEAD commit is not a 40-hex sha: $sha" }
$targetChild = Get-FullPathSafe (Join-Path $releaseRoot $sha)
if (-not (Test-PathInside -Base $releaseRoot -Candidate $targetChild)) {
  throw "Refusing to publish outside the release root: $targetChild"
}
if ((Split-Path $targetChild -Leaf) -ne $sha) {
  throw "Publish leaf must equal the HEAD commit: $targetChild"
}
$nmTool = Resolve-RiscvTool $policy.toolchain_tools.nm
Write-Ok "Publish target: release/$sha"
Write-Ok "Pinned nm: $nmTool"

if ((Test-Path $targetChild) -and -not $Force) {
  throw "release/$sha already exists. Re-run with -Force to replace ONLY that exact validated path."
}

# ===========================================================================
# 4. Delete .pio once (unless -SkipBuild).
# ===========================================================================
$pioDir = Join-Path $repoRoot '.pio'
if ($SkipBuild) {
  Write-Warning "SkipBuild: reusing existing .pio artifacts (NON-CERTIFIED build)."
} else {
  Write-Step "Clean .pio"
  if (Test-Path $pioDir) {
    if (-not (Test-PathInside -Base $repoRoot -Candidate $pioDir)) { throw "refusing to delete $pioDir" }
    Remove-Item -LiteralPath $pioDir -Recurse -Force
  }
  Write-Ok ".pio removed"
}

# ===========================================================================
# 5. Required source gates (unless -SkipSourceGates).
# ===========================================================================
$gateResults = New-Object System.Collections.Generic.List[object]
if ($SkipSourceGates) {
  Write-Warning "SkipSourceGates: NOT running required source gates (NON-CERTIFIED build)."
  foreach ($g in $policy.required_source_gates) {
    $gateResults.Add([ordered]@{ id = $g.id; script = $g.script; status = 'skipped'; summary = $g.summary })
  }
} else {
  Write-Step "Source gates"
  foreach ($g in $policy.required_source_gates) {
    $gateScript = Join-Path $repoRoot ($g.script -replace '/', '\')
    if (-not (Test-Path $gateScript)) { throw "Required gate script not found: $gateScript" }
    Write-Host "  --- $($g.id) ($($g.script)) ---"
    # The repo's gates fail closed by THROWING (each runs under
    # $ErrorActionPreference='Stop'), which propagates here and aborts the release.
    # We deliberately do NOT inspect $LASTEXITCODE: a passing gate can leave a
    # stale non-zero code from an internal native command it handled itself.
    & $gateScript
    $gateResults.Add([ordered]@{ id = $g.id; script = $g.script; status = 'passed'; summary = $g.summary })
    Write-Ok "gate '$($g.id)' passed"
  }
}

# ===========================================================================
# 6. Build + merge the five envs; capture separate build/merge logs.
# ===========================================================================
Write-Step "Build + merge five firmware environments"
$buildInfo = @{}
foreach ($name in $envNames) {
  $buildDir = Join-Path $pioDir "build\$name"
  if ($SkipBuild) {
    if (-not (Test-Path (Join-Path $buildDir 'firmware.elf'))) {
      throw "SkipBuild: $name has no firmware.elf; cannot reuse. Run without -SkipBuild."
    }
    $buildLog = "(SkipBuild: reused existing artifacts)"
    $mergeLog = "(SkipBuild: reused existing artifacts)"
  } else {
    Write-Host "  Building $name ..."
    $buildLog = (& $pio run -e $name 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Write-Host $buildLog; throw "Build failed for env '$name'." }
    Write-Host "  Merging $name ..."
    $mergeLog = (& $pio run -t merge -e $name 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Write-Host $mergeLog; throw "Merge failed for env '$name'." }
  }
  $buildInfo[$name] = [ordered]@{ buildDir = $buildDir; buildLog = $buildLog; mergeLog = $mergeLog }
  Write-Ok "$name built + merged"
}

# ===========================================================================
# 7. Enforce EXACT approved resource/file-size policy.
# ===========================================================================
Write-Step "Enforce exact resource/file-size policy"
$tolerance = [int]$policy.resource_tolerance_bytes
foreach ($envPolicy in $policy.environments) {
  $name = $envPolicy.name
  $bd = $buildInfo[$name].buildDir
  $firmwareBin = Join-Path $bd 'firmware.bin'
  $mergedBin = Join-Path $bd 'firmware-merged.bin'
  foreach ($p in @($firmwareBin, $mergedBin, (Join-Path $bd 'firmware.elf'), (Join-Path $bd 'firmware.map'))) {
    if (-not (Test-Path $p)) { throw "Expected build artifact missing for '$name': $p" }
  }

  # Linker-reported RAM/flash are parsed from the build log's "Checking size" block.
  $ram = $null; $flash = $null
  if (-not $SkipBuild) {
    $logText = $buildInfo[$name].buildLog
    $ramM = [regex]::Match($logText, 'RAM:\s+\[[^\]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes')
    $flashM = [regex]::Match($logText, 'Flash:\s+\[[^\]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes')
    if ($ramM.Success) { $ram = [int]$ramM.Groups[1].Value }
    if ($flashM.Success) { $flash = [int]$flashM.Groups[1].Value }
  }
  $firmwareBinSize = (Get-Item $firmwareBin).Length
  $mergedBinSize = (Get-Item $mergedBin).Length

  $exp = $envPolicy.resources
  function Assert-Size([string]$label, $actual, $expected) {
    if ($null -eq $actual) { throw "[$name] could not determine $label (need a clean build; not available with -SkipBuild)." }
    if ([math]::Abs([int64]$actual - [int64]$expected) -gt $tolerance) {
      throw "[$name] $label mismatch: expected $expected, got $actual (tolerance $tolerance)."
    }
  }
  if (-not $SkipBuild) {
    Assert-Size 'RAM' $ram $exp.ram
    Assert-Size 'flash' $flash $exp.flash
  }
  Assert-Size 'firmware.bin' $firmwareBinSize $exp.firmware_bin
  Assert-Size 'firmware-merged.bin' $mergedBinSize $exp.merged_bin

  $buildInfo[$name].ram = $ram
  $buildInfo[$name].flash = $flash
  $buildInfo[$name].firmwareBinSize = [int64]$firmwareBinSize
  $buildInfo[$name].mergedBinSize = [int64]$mergedBinSize
  $skipNote = ''
  if ($SkipBuild) { $skipNote = ' (RAM/flash skipped)' }
  Write-Ok ("$name sizes exact: bin=$firmwareBinSize merged=$mergedBinSize" + $skipNote)
}

# ===========================================================================
# 8. Binary proof engine (nm symbol evidence + binary-safe byte scan).
#    Any failing invariant aborts BEFORE staging/publishing.
# ===========================================================================
Write-Step "Current-head ELF/binary proofs"

$workerTokens = @('workerTask', 's_worker_stack', 's_worker_tcb', 's_request_q', 's_result_q', 'workerCancel')
$diagSymTokens = @('g_diag_last_fetch_ms', 'radarDisplayLastDiagnostics')
$heapApiTokens = @('getFreeHeap', 'getMinFreeHeap', 'getMaxAllocHeap', 'heap_caps_get_minimum_free_size')
$heapSharedTokens = @('heap_caps_get_free_size', 'heap_caps_get_largest_free_block')
$evidenceTokens = @($workerTokens + $diagSymTokens + $heapApiTokens + $heapSharedTokens)

$diagStringNeedles = [ordered]@{
  'diag: fetch_ms='  = (Get-AsciiBytesWithTrailer -Text 'diag: fetch_ms=')
  'diag: render_us=' = (Get-AsciiBytesWithTrailer -Text 'diag: render_us=')
}
$logStringNeedles = [ordered]@{
  'Plane Radar\n (startup log)' = (Get-AsciiBytesWithTrailer -Text 'Plane Radar' -Trailer @([byte]10))
  'radar: rendering mode: '     = (Get-AsciiBytesWithTrailer -Text 'radar: rendering mode: ')
  'Distance units: '            = (Get-AsciiBytesWithTrailer -Text 'Distance units: ')
  'Runway overlay: '            = (Get-AsciiBytesWithTrailer -Text 'Runway overlay: ')
  'Smooth font load failed'     = (Get-AsciiBytesWithTrailer -Text 'Smooth font load failed -- using bitmap fallback')
}
$planeSubstrNeedle = (Get-AsciiBytesWithTrailer -Text 'Plane Radar')

function Get-EnvFlags([string]$name) {
  $ep = $policy.environments | Where-Object { $_.name -eq $name } | Select-Object -First 1
  return [pscustomobject]@{
    Worker = [bool]$ep.options.worker
    Diag   = [bool]$ep.options.diagnostics
    Quiet  = ([int]$ep.options.log_level -eq 0)
  }
}
function YesNo([bool]$b) { if ($b) { return 'present' } else { return 'absent' } }

$invariants = New-Object System.Collections.Generic.List[object]
$nmEvidence = @{}

foreach ($name in $envNames) {
  $bd = $buildInfo[$name].buildDir
  $flags = Get-EnvFlags $name
  $nmLines = Get-NmLines -NmPath $nmTool -ElfPath (Join-Path $bd 'firmware.elf')
  $nmEvidence[$name] = Select-NmEvidenceLines -NmLines $nmLines -Tokens $evidenceTokens
  $hay = Get-Latin1Haystack (Join-Path $bd 'firmware.bin')

  # -- worker/integration symbols (default binary links ZERO of these) --------
  foreach ($tok in $workerTokens) {
    $present = Test-NmSymbolPresent $nmLines $tok
    $invariants.Add((New-InvariantResult -Id "worker-sym/$name/$tok" -Category 'worker-symbol' -Environment $name `
      -Description "Worker/integration symbol '$tok' linkage" `
      -Expectation (YesNo $flags.Worker) -Observed (YesNo $present) -Pass ($present -eq $flags.Worker)))
  }
  # -- worker stack: exactly one symbol of exactly 8192 bytes in worker builds --
  $stackSizes = Get-NmSymbolSizes $nmLines 's_worker_stack'
  $expCount = 0; if ($flags.Worker) { $expCount = 1 }
  $stackOk = ($stackSizes.Count -eq $expCount)
  if ($flags.Worker) { $stackOk = ($stackSizes.Count -eq 1 -and $stackSizes[0] -eq 8192) }
  $invariants.Add((New-InvariantResult -Id "worker-stack/$name" -Category 'worker-stack' -Environment $name `
    -Description "s_worker_stack symbol count/size (0x2000 = 8192 bytes)" `
    -Expectation ("count=$expCount" + $(if ($flags.Worker) { ', size=8192' } else { '' })) `
    -Observed ("count=$($stackSizes.Count), sizes=[$($stackSizes -join ',')]") -Pass $stackOk))

  # -- diagnostic symbols ------------------------------------------------------
  foreach ($tok in $diagSymTokens) {
    $present = Test-NmSymbolPresent $nmLines $tok
    $invariants.Add((New-InvariantResult -Id "diag-sym/$name/$tok" -Category 'diag-symbol' -Environment $name `
      -Description "Diagnostic symbol '$tok'" `
      -Expectation (YesNo $flags.Diag) -Observed (YesNo $present) -Pass ($present -eq $flags.Diag)))
  }
  # -- heap diagnostic API indicators (ESP.getFreeHeap/getMinFreeHeap/getMaxAllocHeap) --
  foreach ($tok in $heapApiTokens) {
    $present = Test-NmSymbolPresent $nmLines $tok
    $invariants.Add((New-InvariantResult -Id "heap-api/$name/$tok" -Category 'heap-api' -Environment $name `
      -Description "Heap diagnostic API indicator '$tok'" `
      -Expectation (YesNo $flags.Diag) -Observed (YesNo $present) -Pass ($present -eq $flags.Diag)))
  }
  # -- shared low-level heap symbols: present in ALL builds (NOT isolation indicators) --
  foreach ($tok in $heapSharedTokens) {
    $present = Test-NmSymbolPresent $nmLines $tok
    $invariants.Add((New-InvariantResult -Id "heap-shared/$name/$tok" -Category 'heap-shared' -Environment $name `
      -Description "Shared framework heap symbol '$tok' (present in every build; NOT a diag indicator)" `
      -Expectation 'present' -Observed (YesNo $present) -Pass ($present)))
  }

  # -- diagnostic binary format strings ---------------------------------------
  foreach ($label in $diagStringNeedles.Keys) {
    $present = Test-HaystackContainsBytes $hay $diagStringNeedles[$label]
    $invariants.Add((New-InvariantResult -Id "diag-str/$name/$label" -Category 'diag-string' -Environment $name `
      -Description "Diagnostic binary string '$label'" `
      -Expectation (YesNo $flags.Diag) -Observed (YesNo $present) -Pass ($present -eq $flags.Diag)))
  }
  # -- quiet build lacks Serial logging strings (non-quiet builds contain them) --
  foreach ($label in $logStringNeedles.Keys) {
    $present = Test-HaystackContainsBytes $hay $logStringNeedles[$label]
    $expected = -not $flags.Quiet
    $invariants.Add((New-InvariantResult -Id "quiet-log/$name/$label" -Category 'quiet-log' -Environment $name `
      -Description "Serial logging string '$label'" `
      -Expectation (YesNo $expected) -Observed (YesNo $present) -Pass ($present -eq $expected)))
  }
  # -- baseline: plain 'Plane Radar' (portal HTML) is present in ALL builds -----
  $planePresent = Test-HaystackContainsBytes $hay $planeSubstrNeedle
  $invariants.Add((New-InvariantResult -Id "quiet-log-baseline/$name" -Category 'quiet-log-baseline' -Environment $name `
    -Description "Plain 'Plane Radar' substring (portal HTML; present in every build, incl. quiet -- proves why plain substring is NOT a logging indicator)" `
    -Expectation 'present' -Observed (YesNo $planePresent) -Pass ($planePresent)))
}

$failed = @($invariants | Where-Object { $_.status -ne 'pass' })
$proofOverall = 'pass'; if ($failed.Count -gt 0) { $proofOverall = 'fail' }
Write-Ok "proof invariants: $($invariants.Count) total, $($invariants.Count - $failed.Count) pass, $($failed.Count) fail"
if ($failed.Count -gt 0) {
  Write-Host "FAILED INVARIANTS:" -ForegroundColor Red
  foreach ($f in $failed) { Write-Host ("  [{0}] {1}: expected {2}, observed {3}" -f $f.environment, $f.id, $f.expectation, $f.observed) -ForegroundColor Red }
  throw "Binary proof failed ($($failed.Count) invariant(s)); aborting before publishing."
}

# ===========================================================================
# 9. Stage the package (under release/, then atomically publish).
# ===========================================================================
Write-Step "Stage release package"
$generatedUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
$stageLeaf = ".stage-$sha-$([System.IO.Path]::GetRandomFileName())"
$stageDir = Get-FullPathSafe (Join-Path $releaseRoot $stageLeaf)
if (-not (Test-PathInside -Base $releaseRoot -Candidate $stageDir)) { throw "staging path escaped release root" }
if (Test-Path $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

$copyArtifacts = @($policy.copied_artifacts)
foreach ($name in $envNames) {
  $bd = $buildInfo[$name].buildDir
  $envStage = Join-Path $stageDir $name
  New-Item -ItemType Directory -Path $envStage -Force | Out-Null
  Copy-Item (Join-Path $bd 'firmware.bin')        (Join-Path $envStage 'firmware.bin')        -Force
  Copy-Item (Join-Path $bd 'firmware-merged.bin') (Join-Path $envStage 'firmware-merged.bin') -Force
  Copy-Item (Join-Path $bd 'firmware.elf')        (Join-Path $envStage 'firmware.elf')        -Force
  Copy-Item (Join-Path $bd 'firmware.map')        (Join-Path $envStage 'firmware.map')        -Force
  Write-TextFileLf (Join-Path $envStage 'build.log') ($buildInfo[$name].buildLog)
  Write-TextFileLf (Join-Path $envStage 'merge.log') ($buildInfo[$name].mergeLog)
  $nmHeader = "# nm -C -S -t d evidence for env '$name' (worker/diag/heap symbols only)`n" +
              "# tool: $nmTool`n"
  Write-TextFileLf (Join-Path $envStage 'nm-symbols.txt') ($nmHeader + (($nmEvidence[$name]) -join "`n"))
}

# -- binary-proof.json / binary-proof.txt (written BEFORE the manifest) --------
$proofCategories = [ordered]@{}
foreach ($inv in $invariants) {
  if (-not $proofCategories.Contains($inv.category)) { $proofCategories[$inv.category] = 0 }
  $proofCategories[$inv.category] = [int]$proofCategories[$inv.category] + 1
}
$proofJson = [ordered]@{
  schema        = 'plane-radar/binary-proof'
  schemaVersion = 1
  generatedUtc  = $generatedUtc
  commit        = $sha
  branch        = $git.Branch
  nmTool        = (Split-Path $nmTool -Leaf)
  overall       = $proofOverall
  total         = $invariants.Count
  passed        = ($invariants.Count - $failed.Count)
  failed        = $failed.Count
  categories    = $proofCategories
  note          = 'Binds the EXACT current-head binaries only. Does NOT claim raw byte equivalence to Phase 10/11 artifacts.'
  invariants    = $invariants.ToArray()
}
Write-JsonFileLf (Join-Path $stageDir 'binary-proof.json') $proofJson

$txt = New-Object System.Text.StringBuilder
[void]$txt.AppendLine("Plane Radar -- current-head binary proof")
[void]$txt.AppendLine("commit:   $sha")
[void]$txt.AppendLine("branch:   $($git.Branch)")
[void]$txt.AppendLine("generated: $generatedUtc UTC")
[void]$txt.AppendLine("nm tool:  $(Split-Path $nmTool -Leaf)")
[void]$txt.AppendLine("overall:  $proofOverall ($($invariants.Count) invariants, $($failed.Count) failed)")
[void]$txt.AppendLine("")
[void]$txt.AppendLine("This package binds the EXACT current-head binaries only; it does NOT claim")
[void]$txt.AppendLine("raw byte equivalence to any prior phase's artifacts.")
[void]$txt.AppendLine("")
$lastCat = ''
foreach ($inv in ($invariants | Sort-Object category, environment, id)) {
  if ($inv.category -ne $lastCat) { [void]$txt.AppendLine("[$($inv.category)]"); $lastCat = $inv.category }
  [void]$txt.AppendLine(("  {0,-5} {1,-22} {2}" -f $inv.status.ToUpper(), $inv.environment, $inv.description))
  [void]$txt.AppendLine(("        expect={0}  observed={1}" -f $inv.expectation, $inv.observed))
}
Write-TextFileLf (Join-Path $stageDir 'binary-proof.txt') ($txt.ToString())

# ===========================================================================
# 10. Manifest (artifacts flat map + proof/gate summary + default-artifact role).
# ===========================================================================
Write-Step "Write manifest + CHECKSUMS"

# Flat artifacts map: every staged file (manifest.json + CHECKSUMS.sha256 are not
# written yet, so nothing to exclude). Ordinal-sorted for deterministic order.
$artifactsMap = New-PackageArtifactsMap -PackageDir $stageDir
$defaultRel = $policy.default_artifact.relative_path
if (-not $artifactsMap.Contains($defaultRel)) { throw "default artifact '$defaultRel' missing from package." }

$envManifest = New-Object System.Collections.Generic.List[object]
foreach ($envPolicy in $policy.environments) {
  $name = $envPolicy.name
  $bi = $buildInfo[$name]
  $envArtifacts = [ordered]@{}
  foreach ($rel in $artifactsMap.Keys) {
    if ($rel.StartsWith("$name/")) { $envArtifacts[$rel.Substring($name.Length + 1)] = $artifactsMap[$rel] }
  }
  $envManifest.Add([ordered]@{
    name            = $name
    role            = $envPolicy.role
    evaluation_only = [bool]$envPolicy.evaluation_only
    options         = [ordered]@{ worker = [bool]$envPolicy.options.worker; diagnostics = [bool]$envPolicy.options.diagnostics; log_level = [int]$envPolicy.options.log_level }
    resources       = [ordered]@{ ram = $bi.ram; flash = $bi.flash; firmware_bin = $bi.firmwareBinSize; merged_bin = $bi.mergedBinSize }
    artifacts       = $envArtifacts
  })
}

$manifest = [ordered]@{
  schema        = 'plane-radar/release-manifest'
  schemaVersion = 1
  generatedUtc  = $generatedUtc
  git           = [ordered]@{ commit = $sha; short = $git.ShortCommit; branch = $git.Branch; tracked_clean = (-not $git.TrackedDirty); untracked_files = $git.UntrackedCount }
  local_only    = [ordered]@{ no_remote = (-not $git.HasRemote); release_path = "release/$sha" }
  pins          = [ordered]@{
    platformio_core             = $policy.pins.platformio_core
    platformio_core_verified    = ("$pioVersion".Trim())
    platform                    = $policy.pins.platform
    framework                   = $policy.pins.framework
    framework_arduinoespressif32 = $policy.pins.framework_arduinoespressif32
    toolchain_riscv32_esp       = $policy.pins.toolchain_riscv32_esp
    tool_esptoolpy              = $policy.pins.tool_esptoolpy
    board                       = $policy.pins.board
    mcu                         = $policy.pins.mcu
    flash_size                  = $policy.pins.flash_size
    app_offset                  = $policy.pins.app_offset
    dependencies                = $policy.pins.dependencies
  }
  airport_source_commit = $policy.airport_data.source_commit
  toolchain     = [ordered]@{ nm = (Split-Path $nmTool -Leaf) }
  default_artifact = [ordered]@{
    environment   = $policy.default_artifact.environment
    relative_path = $defaultRel
    role          = 'default-release'
    size          = $artifactsMap[$defaultRel].size
    sha256        = $artifactsMap[$defaultRel].sha256
    note          = 'The ONLY default release image (flash at offset 0x0).'
  }
  worker_note   = 'supermini-worker and supermini-worker-diag are EVALUATION-ONLY prototype images; they are NOT the default release firmware until the hardware WORKER PROMOTION gate passes.'
  gates         = $gateResults.ToArray()
  tests         = [ordered]@{ native_test = '606 cases across 41 suite runs (native 580/39 + native-diag 5/1 + native-gfx 21/1)'; gates_run = (-not $SkipSourceGates) }
  build         = [ordered]@{ clean_build = (-not $SkipBuild); certified = ((-not $SkipBuild) -and (-not $SkipSourceGates) -and (-not $AllowDirtyWorktree)) }
  proof_summary = [ordered]@{ overall = $proofOverall; total = $invariants.Count; passed = ($invariants.Count - $failed.Count); failed = $failed.Count }
  environments  = $envManifest.ToArray()
  artifacts     = $artifactsMap
  binding_note  = 'This package binds the EXACT current-head binaries only; it does NOT claim raw byte equivalence to Phase 10/11 artifacts.'
}
Write-JsonFileLf (Join-Path $stageDir 'manifest.json') $manifest

# -- CHECKSUMS.sha256: covers manifest + every package file EXCEPT itself, sorted
# ordinally by forward-slash relative path. Shared helper => identical logic in
# build-release and the verifier's self-test fixture builder.
Write-PackageChecksums -PackageDir $stageDir

# ===========================================================================
# 11. Publish atomically (replace only the exact validated child on -Force).
# ===========================================================================
Write-Step "Publish"
if (Test-Path $targetChild) {
  if (-not $Force) { throw "release/$sha already exists (unexpected)." }
  if (-not (Test-PathInside -Base $releaseRoot -Candidate $targetChild)) { throw "refusing to delete unresolved path" }
  if ((Split-Path $targetChild -Leaf) -ne $sha) { throw "refusing to delete non-sha leaf" }
  Remove-Item -LiteralPath $targetChild -Recurse -Force
  Write-Ok "removed prior release/$sha (--Force)"
}
Move-Item -LiteralPath $stageDir -Destination $targetChild
Write-Ok "published release/$sha"

# ===========================================================================
# 12. Summary.
# ===========================================================================
Write-Step "Summary"
Write-Host "Release package: release/$sha"
Write-Host "Default artifact: $defaultRel"
Write-Host ("  size={0}  sha256={1}" -f $artifactsMap[$defaultRel].size, $artifactsMap[$defaultRel].sha256)
Write-Host "Per-env firmware-merged.bin SHA-256:"
foreach ($name in $envNames) {
  $rel = "$name/firmware-merged.bin"
  Write-Host ("  {0,-24} {1}" -f $name, $artifactsMap[$rel].sha256)
}
Write-Host "Proof: $proofOverall ($($invariants.Count) invariants). Certified build: $((-not $SkipBuild) -and (-not $SkipSourceGates) -and (-not $AllowDirtyWorktree))"
Write-Host "Verify with:  .\scripts\verify-release.ps1 -Path release/$sha"
