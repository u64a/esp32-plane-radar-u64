<#
.SYNOPSIS
  Phase 12 fail-closed, ALWAYS-CERTIFIED release builder for the ESP32-C3
  Plane Radar firmware.

.DESCRIPTION
  Produces a reproducible, self-describing, re-verifiable, CERTIFIED release
  package for all five firmware environments under release/<full-git-sha>/, with
  persistent current-head ELF/binary proofs, a manifest, and a checksum manifest.

  There are NO certification bypasses. Every run unconditionally:
    * requires PlatformIO Core EXACTLY the pinned version;
    * requires git HEAD, a clean tracked/index worktree, NO untracked files, and
      NO git remote in the isolated certification checkout;
    * publishes strictly to <repo>/release/<40-hex HEAD>/ (no override);
    * deletes .pio ONCE and freshly builds + merges exactly the five envs;
    * runs EVERY required source gate;
    * enforces the approved EXACT resource/file sizes;
    * verifies and RECORDS the OBSERVED installed toolchain/library versions and
      fails on any drift from the pins;
    * re-derives the current-head binary proofs with the pinned nm;
  and only if every invariant passes does it stage and publish the CERTIFIED
  package. Iteration/diagnosis is done with direct `pio` commands; this builder is
  not a diagnostic tool.

  All recursive deletions (.pio, the staging dir, and an existing target on
  -Force) go through the reparse-point-safe Remove-TreeSafe helper, which proves
  the exact allowed base/leaf and refuses to delete through a junction/symlink.

  This package binds the EXACT current-head binaries only; it does NOT claim raw
  byte equivalence to any prior phase's artifacts.

.PARAMETER Force
  Replace an already-published release/<sha>/ directory. Only that exact validated
  child path is removed (reparse-point-safe); a broad/unresolved path is never
  deleted.
#>
[CmdletBinding()]
param(
  [switch]$Force
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
# 0. Load the tracked release policy from the EXACT git HEAD commit blob (never
#    the editable working tree) and record its SHA-256. A dirty working-tree
#    policy must NOT influence certification.
# ===========================================================================
$git = Get-RepoGitState $repoRoot
if (-not (Test-GitCommitPresent -RepoRoot $repoRoot -Commit $git.Commit)) {
  throw "HEAD commit $($git.Commit) is not present as a git object; cannot load tracked policy."
}
$gp = Get-GitPolicy -RepoRoot $repoRoot -Commit $git.Commit -RelPath 'scripts/release-policy.json'
$policy = $gp.Object
$policySha = $gp.Sha256
$envNames = @($policy.environments | ForEach-Object { $_.name })

# ===========================================================================
# 1. PlatformIO must be EXACTLY the pinned version.
# ===========================================================================
Write-Step "Preconditions"
$pio = Resolve-PlatformIoExe
# Materialize the full output (and capture the exit code) BEFORE selecting a line:
# piping a native command straight into `Select-Object -First 1` stops the pipeline
# early, terminates pio.exe, and corrupts $LASTEXITCODE.
$pioVersionRaw = @(& $pio --version 2>&1)
$pioExit = $LASTEXITCODE
$pioVersion = ($pioVersionRaw | Select-Object -First 1)
$expectedPio = "PlatformIO Core, version $($policy.pins.platformio_core)"
if ($pioExit -ne 0 -or "$pioVersion".Trim() -ne $expectedPio) {
  throw "PlatformIO $($policy.pins.platformio_core) is required; found: $pioVersion (exit $pioExit)"
}
Write-Ok "PlatformIO $($policy.pins.platformio_core)"

# ===========================================================================
# 2. Git: HEAD exists, clean tracked+index, no untracked, no remote in the
#    isolated certification checkout (ALWAYS).
# ===========================================================================
if ($git.HasRemote) {
  throw "A git remote is configured ($($git.RemoteCount)); certified releases require an isolated no-remote checkout. Aborting."
}
if ($git.TrackedDirty) {
  throw "The tracked/index worktree is not clean:`n$($git.TrackedLines -join "`n")"
}
if ($git.UntrackedCount -gt 0) {
  throw "Untracked files are present (release must build from a pristine worktree):`n$($git.UntrackedLines -join "`n")"
}
Write-Ok "git HEAD $($git.ShortCommit) on '$($git.Branch)': clean, no untracked, no remote"
Write-Ok "policy: scripts/release-policy.json@$($git.ShortCommit) sha256=$policySha"

# ===========================================================================
# 3. Resolve + validate the publish target (ALWAYS <repo>/release/<40-hex sha>).
# ===========================================================================
$releaseRoot = Get-FullPathSafe (Join-Path $repoRoot $policy.local_only.release_root_relative)
if (-not (Test-PathInside -Base $repoRoot -Candidate $releaseRoot -AllowEqual)) {
  throw "Release root '$releaseRoot' is not inside the repository."
}
# Reparse-point defence: refuse if the release root is itself a junction/symlink
# (a planted reparse could redirect staging/publish outside the tree).
if ((Test-Path $releaseRoot) -and (Test-IsReparsePoint $releaseRoot)) {
  throw "Release root '$releaseRoot' is a symlink/junction/reparse point; refusing to publish."
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
Write-Ok "Publish target: release/$sha"

if ((Test-Path $targetChild) -and -not $Force) {
  throw "release/$sha already exists. Re-run with -Force to replace ONLY that exact validated path."
}

# ===========================================================================
# 4. Delete .pio once (reparse-point-safe).
# ===========================================================================
Write-Step "Clean .pio"
$pioDir = Join-Path $repoRoot '.pio'
Remove-TreeSafe -Path $pioDir -AllowedBase $repoRoot -RequireLeaf '.pio'
Write-Ok ".pio removed"

# ===========================================================================
# 5. Required source gates (ALWAYS run; a throw aborts the release).
# ===========================================================================
Write-Step "Source gates"
$gateResults = New-Object System.Collections.Generic.List[object]
foreach ($g in $policy.required_source_gates) {
  $gateScript = Join-Path $repoRoot ($g.script -replace '/', '\')
  if (-not (Test-Path $gateScript)) { throw "Required gate script not found: $gateScript" }
  Write-Host "  --- $($g.id) ($($g.script)) ---"
  # The repo's gates fail closed by THROWING (each runs under
  # $ErrorActionPreference='Stop'), which propagates here and aborts the release.
  # We deliberately do NOT inspect $LASTEXITCODE: a passing gate can leave a stale
  # non-zero code from an internal native command it handled itself.
  & $gateScript
  $gateResults.Add([ordered]@{ id = $g.id; script = $g.script; status = 'passed'; summary = $g.summary })
  Write-Ok "gate '$($g.id)' passed"
}

# ===========================================================================
# 6. Build + merge the five envs; capture separate build/merge logs.
# ===========================================================================
Write-Step "Build + merge five firmware environments"
$buildInfo = @{}
foreach ($name in $envNames) {
  $buildDir = Join-Path $pioDir "build\$name"
  Write-Host "  Building $name ..."
  $buildLog = (& $pio run -e $name 2>&1 | Out-String)
  if ($LASTEXITCODE -ne 0) { Write-Host $buildLog; throw "Build failed for env '$name'." }
  Write-Host "  Merging $name ..."
  $mergeLog = (& $pio run -t merge -e $name 2>&1 | Out-String)
  if ($LASTEXITCODE -ne 0) { Write-Host $mergeLog; throw "Merge failed for env '$name'." }
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
  $logText = $buildInfo[$name].buildLog
  $ramM = [regex]::Match($logText, 'RAM:\s+\[[^\]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes')
  $flashM = [regex]::Match($logText, 'Flash:\s+\[[^\]]*\]\s+[\d.]+%\s+\(used\s+(\d+)\s+bytes')
  $ram = $null; $flash = $null
  if ($ramM.Success) { $ram = [int]$ramM.Groups[1].Value }
  if ($flashM.Success) { $flash = [int]$flashM.Groups[1].Value }
  $firmwareBinSize = (Get-Item $firmwareBin).Length
  $mergedBinSize = (Get-Item $mergedBin).Length
  $firmwareElf = Join-Path $bd 'firmware.elf'
  $firmwareElfSize = (Get-Item $firmwareElf).Length

  $exp = $envPolicy.resources
  function Assert-Size([string]$label, $actual, $expected) {
    if ($null -eq $actual) { throw "[$name] could not determine $label (need a clean build)." }
    if ([math]::Abs([int64]$actual - [int64]$expected) -gt $tolerance) {
      throw "[$name] $label mismatch: expected $expected, got $actual (tolerance $tolerance)."
    }
  }
  Assert-Size 'RAM' $ram $exp.ram
  Assert-Size 'flash' $flash $exp.flash
  Assert-Size 'firmware.bin' $firmwareBinSize $exp.firmware_bin
  Assert-Size 'firmware-merged.bin' $mergedBinSize $exp.merged_bin
  # firmware.elf size is a deterministic (reproducible) build-metadata anchor.
  Assert-Size 'firmware.elf' $firmwareElfSize (Get-PsObjectProperty $exp 'firmware_elf')

  # Exact policy SHA-256 anchor: the built firmware.elf, firmware.bin AND the
  # SHIPPED firmware-merged.bin must equal the tracked policy hashes for this
  # env; AND firmware.bin's app-descriptor embedded ELF SHA-256 must equal
  # SHA-256(firmware.elf) so the ELF proof input is bound to the pinned image.
  $polSha = Get-PsObjectProperty $envPolicy 'sha256'
  if ($null -eq $polSha) { throw "[$name] policy has no sha256 anchor (firmware_elf/firmware_bin/merged_bin)." }
  $expFwSha = [string](Get-PsObjectProperty $polSha 'firmware_bin')
  $expMgSha = [string](Get-PsObjectProperty $polSha 'merged_bin')
  $expElfSha = [string](Get-PsObjectProperty $polSha 'firmware_elf')
  $actFwSha = Get-Sha256Hex $firmwareBin
  $actMgSha = Get-Sha256Hex $mergedBin
  $actElfSha = Get-Sha256Hex $firmwareElf
  if (-not ((Test-Sha256Hex $expElfSha) -and ($actElfSha -eq $expElfSha))) {
    throw "[$name] firmware.elf SHA-256 mismatch: expected $expElfSha, got $actElfSha."
  }
  if (-not ((Test-Sha256Hex $expFwSha) -and ($actFwSha -eq $expFwSha))) {
    throw "[$name] firmware.bin SHA-256 mismatch: expected $expFwSha, got $actFwSha."
  }
  if (-not ((Test-Sha256Hex $expMgSha) -and ($actMgSha -eq $expMgSha))) {
    throw "[$name] firmware-merged.bin SHA-256 mismatch: expected $expMgSha, got $actMgSha."
  }
  # elf-binding: firmware.bin[app_descriptor.elf_sha256_offset] == SHA-256(elf).
  $ad = Get-PsObjectProperty $policy 'app_descriptor'
  $adOff = 176; $adLen = 32
  if ($ad) { $o = Get-PsObjectProperty $ad 'elf_sha256_offset'; if ($null -ne $o) { $adOff = [int]$o }; $l = Get-PsObjectProperty $ad 'elf_sha256_length'; if ($null -ne $l) { $adLen = [int]$l } }
  $binBytesForBind = [System.IO.File]::ReadAllBytes($firmwareBin)
  if (($adOff -lt 0) -or (($adOff + $adLen) -gt $binBytesForBind.Length)) { throw "[$name] app-descriptor ELF-SHA offset $adOff out of range for firmware.bin." }
  $segForBind = New-Object byte[] $adLen; [Array]::Copy($binBytesForBind, $adOff, $segForBind, 0, $adLen)
  $embeddedElfSha = (($segForBind | ForEach-Object { $_.ToString('x2') }) -join '')
  if ($embeddedElfSha -ne $actElfSha) {
    throw "[$name] elf-binding failed: firmware.bin embedded ELF SHA-256 ($embeddedElfSha) != SHA-256(firmware.elf) ($actElfSha)."
  }

  $buildInfo[$name].ram = $ram
  $buildInfo[$name].flash = $flash
  $buildInfo[$name].firmwareBinSize = [int64]$firmwareBinSize
  $buildInfo[$name].mergedBinSize = [int64]$mergedBinSize
  $buildInfo[$name].firmwareElfSize = [int64]$firmwareElfSize
  Write-Ok "$name sizes+SHA exact: elf=$firmwareElfSize bin=$firmwareBinSize merged=$mergedBinSize (elf-binding OK)"
}

# ===========================================================================
# 8. Verify + record OBSERVED installed toolchain/library versions vs pins.
# ===========================================================================
Write-Step "Observed toolchain/library versions"
$libDepsDir = Join-Path $repoRoot ('.pio\libdeps\' + $policy.default_artifact.environment)
$observed = Get-ObservedToolVersions -Policy $policy -LibDepsDir $libDepsDir
$expPlatformVer = Get-PolicyPlatformVersion ([string]$policy.pins.platform)
$versionMismatch = New-Object System.Collections.Generic.List[string]
if ([string]$observed.platform_version -ne $expPlatformVer) { $versionMismatch.Add("platform: expected $expPlatformVer, observed $($observed.platform_version)") }
if ([string]$observed.framework_arduinoespressif32 -ne [string]$policy.pins.framework_arduinoespressif32) { $versionMismatch.Add("framework-arduinoespressif32: expected $($policy.pins.framework_arduinoespressif32), observed $($observed.framework_arduinoespressif32)") }
if ([string]$observed.toolchain_riscv32_esp -ne [string]$policy.pins.toolchain_riscv32_esp) { $versionMismatch.Add("toolchain-riscv32-esp: expected $($policy.pins.toolchain_riscv32_esp), observed $($observed.toolchain_riscv32_esp)") }
if ([string]$observed.tool_esptoolpy -ne [string]$policy.pins.tool_esptoolpy) { $versionMismatch.Add("tool-esptoolpy: expected $($policy.pins.tool_esptoolpy), observed $($observed.tool_esptoolpy)") }
foreach ($dp in $policy.pins.dependencies.PSObject.Properties) {
  $obsVer = ''
  if ($observed.dependencies.Contains($dp.Name)) { $obsVer = [string]$observed.dependencies[$dp.Name] }
  if ($obsVer -ne [string]$dp.Value) { $versionMismatch.Add("$($dp.Name): expected $($dp.Value), observed $obsVer") }
}
if ($versionMismatch.Count -gt 0) {
  throw "Observed installed versions differ from the pinned toolchain/libraries:`n  " + [string]::Join("`n  ", $versionMismatch)
}
Write-Ok "platform=$($observed.platform_version) framework=$($observed.framework_arduinoespressif32) toolchain=$($observed.toolchain_riscv32_esp) esptool=$($observed.tool_esptoolpy)"
Write-Ok "LovyanGFX=$($observed.dependencies['lovyan03/LovyanGFX']) ArduinoJson=$($observed.dependencies['bblanchon/ArduinoJson'])"

# ===========================================================================
# 9. Binary proof engine (shared with the verifier's re-derivation).
#    Any failing invariant aborts BEFORE staging/publishing.
# ===========================================================================
Write-Step "Current-head ELF/binary proofs"
# A cold PlatformIO installation does not contain the RISC-V toolchain until a
# firmware environment has been built. Resolve nm here, after all five builds,
# rather than during preflight so a clean CI runner remains self-bootstrapping.
$nmTool = Resolve-RiscvTool $policy.toolchain_tools.nm
Write-Ok "Pinned nm: $nmTool"
$envElf = @{}; $envBin = @{}; $envMerged = @{}
foreach ($name in $envNames) {
  $bd = $buildInfo[$name].buildDir
  $envElf[$name] = Join-Path $bd 'firmware.elf'
  $envBin[$name] = Join-Path $bd 'firmware.bin'
  $envMerged[$name] = Join-Path $bd 'firmware-merged.bin'
}
$proofResult = Get-BinaryProofInvariants -Policy $policy -EnvElf $envElf -EnvBin $envBin -EnvMerged $envMerged -NmPath $nmTool -RepoRoot $repoRoot
$invariants = @($proofResult.Invariants)
$nmEvidence = $proofResult.NmEvidence
$failed = @($invariants | Where-Object { $_.status -ne 'pass' })
$proofOverall = Get-ProofOverall $invariants
Write-Ok "proof invariants: $($invariants.Count) total, $($invariants.Count - $failed.Count) pass, $($failed.Count) fail"
if ($failed.Count -gt 0) {
  Write-Host "FAILED INVARIANTS:" -ForegroundColor Red
  foreach ($f in $failed) { Write-Host ("  [{0}] {1}: expected {2}, observed {3}" -f $f.environment, $f.id, $f.expectation, $f.observed) -ForegroundColor Red }
  throw "Binary proof failed ($($failed.Count) invariant(s)); aborting before publishing."
}

# ===========================================================================
# 10. Stage the package (under release/, then atomically publish).
#     Re-run the git source-gate IMMEDIATELY before staging to close the
#     source-gate/build TOCTOU window (a gate/tool could have mutated source or
#     the worktree after the initial check).
# ===========================================================================
Write-Step "Stage release package"
$git2 = Get-RepoGitState $repoRoot
if (($git2.Commit -ne $sha) -or ($git2.Branch -ne $git.Branch) -or $git2.HasRemote -or $git2.TrackedDirty -or ($git2.UntrackedCount -gt 0)) {
  throw ("Git state changed after gates/build (source-gate/build TOCTOU): now commit=$($git2.ShortCommit) branch='$($git2.Branch)' remote=$($git2.HasRemote) tracked_dirty=$($git2.TrackedDirty) untracked=$($git2.UntrackedCount); expected commit=$($git.ShortCommit) branch='$($git.Branch)' clean/no-remote. Aborting before publishing.")
}
Write-Ok "git state re-confirmed unchanged ($($git2.ShortCommit) on '$($git2.Branch)')"

$generatedUtc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
$stageLeaf = ".stage-$sha-$([System.IO.Path]::GetRandomFileName())"
$stageDir = Get-FullPathSafe (Join-Path $releaseRoot $stageLeaf)
if (-not (Test-PathInside -Base $releaseRoot -Candidate $stageDir)) { throw "staging path escaped release root" }

# A failed stage/write/publish must leave NO staging debris: the finally cleans
# ONLY this exact validated .stage-* directory (reparse-point-safe).
$published = $false
try {
Remove-TreeSafe -Path $stageDir -AllowedBase $releaseRoot -RequireLeaf $stageLeaf
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

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
$proofCategories = Get-ProofCategoryCounts $invariants
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
  invariants    = $invariants
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
# 11. Manifest (artifacts flat map + proof/gate summary + default-artifact role).
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
    resources       = [ordered]@{ ram = $bi.ram; flash = $bi.flash; firmware_bin = $bi.firmwareBinSize; merged_bin = $bi.mergedBinSize; firmware_elf = $bi.firmwareElfSize }
    artifacts       = $envArtifacts
  })
}

$manifest = [ordered]@{
  schema        = 'plane-radar/release-manifest'
  schemaVersion = 1
  generatedUtc  = $generatedUtc
  git           = [ordered]@{ commit = $sha; short = $git.ShortCommit; branch = $git.Branch; tracked_clean = (-not $git.TrackedDirty); untracked_files = $git.UntrackedCount }
  local_only    = [ordered]@{ no_remote = (-not $git.HasRemote); release_path = "release/$sha" }
  policy_sha256 = $policySha
  pins          = [ordered]@{
    platformio_core              = $policy.pins.platformio_core
    platformio_core_verified     = ("$pioVersion".Trim())
    platform                     = $policy.pins.platform
    framework                    = $policy.pins.framework
    framework_arduinoespressif32 = $policy.pins.framework_arduinoespressif32
    toolchain_riscv32_esp        = $policy.pins.toolchain_riscv32_esp
    tool_esptoolpy               = $policy.pins.tool_esptoolpy
    board                        = $policy.pins.board
    mcu                          = $policy.pins.mcu
    flash_size                   = $policy.pins.flash_size
    app_offset                   = $policy.pins.app_offset
    dependencies                 = $policy.pins.dependencies
    observed                     = [ordered]@{
      platform_version             = $observed.platform_version
      framework_arduinoespressif32 = $observed.framework_arduinoespressif32
      toolchain_riscv32_esp        = $observed.toolchain_riscv32_esp
      tool_esptoolpy               = $observed.tool_esptoolpy
      dependencies                 = $observed.dependencies
      note                         = 'Measured from installed PlatformIO package/library metadata at build time; build fails on any drift from the pins.'
    }
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
  tests         = [ordered]@{ native_test = $policy.tests.native_test_summary; gates_run = $true }
  build         = [ordered]@{ clean_build = $true; certified = $true }
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
# 12. Publish atomically (replace only the exact validated child on -Force).
# ===========================================================================
Write-Step "Publish"
if (Test-Path $targetChild) {
  if (-not $Force) { throw "release/$sha already exists (unexpected)." }
  Remove-TreeSafe -Path $targetChild -AllowedBase $releaseRoot -RequireLeaf $sha
  Write-Ok "removed prior release/$sha (--Force)"
}
Move-Item -LiteralPath $stageDir -Destination $targetChild
$published = $true
Write-Ok "published release/$sha"
}
finally {
  # Any failure before the atomic publish must clean ONLY this exact .stage-*
  # directory -- never leave ignored staging debris under release/.
  if (-not $published -and (Test-Path -LiteralPath $stageDir)) {
    try {
      Remove-TreeSafe -Path $stageDir -AllowedBase $releaseRoot -RequireLeaf $stageLeaf
      Write-Host "  cleaned staging debris: $stageLeaf"
    } catch {
      Write-Host "  WARNING: could not clean staging dir '$stageLeaf': $($_.Exception.Message)" -ForegroundColor Yellow
    }
  }
}

# ===========================================================================
# 13. Summary.
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
Write-Host "Proof: $proofOverall ($($invariants.Count) invariants). Certified build: True"
Write-Host "Verify with:  .\scripts\verify-release.ps1 -Path release/$sha"
