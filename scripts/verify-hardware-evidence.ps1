<#
.SYNOPSIS
  Verify completed hardware acceptance evidence against the acceptance policy,
  bound to a verified local release. Reports pass/fail; NEVER auto-promotes.

.DESCRIPTION
  1. Re-verifies the release package (same core as verify-release.ps1).
  2. Requires hardware-results.json to bind to the EXACT default merged image
     SHA-256 / size / commit, and each item to its own flashed image SHA-256.
  3. For the selected gate (default 'default-release'; use -Gate worker-promotion
     for the separate 72 h worker gate), requires every mandatory item status
     'pass', all required evidence files present + non-empty, all evidence
     filenames safe (no traversal/absolute), and all measured values at/above/
     below their policy thresholds. Fails on pending/blocked/waived/unknown/fail
     unless an item is explicitly informational.

  Worker promotion is a SEPARATE gate: passing it only REPORTS worker readiness.
  This script never changes the default artifact (supermini).

.PARAMETER Path
  The release package directory (release/<sha>), absolute or repo-relative.

.PARAMETER Gate
  Which gate to evaluate: default-release (default), worker-promotion, or all.

.PARAMETER AllowStaleHead
  Permit a release whose commit does not match the current HEAD.

.PARAMETER SelfTest
  Run the isolated tamper self-test (never writes under release/).

.PARAMETER PolicyPath / -HardwarePolicyPath
  Override the release / hardware acceptance policy files.
#>
[CmdletBinding(DefaultParameterSetName = 'Verify')]
param(
  [Parameter(ParameterSetName = 'Verify', Position = 0)]
  [string]$Path,
  [Parameter(ParameterSetName = 'Verify')]
  [ValidateSet('default-release', 'worker-promotion', 'all')]
  [string]$Gate = 'default-release',
  [Parameter(ParameterSetName = 'Verify')]
  [switch]$AllowStaleHead,
  [Parameter(ParameterSetName = 'SelfTest')]
  [switch]$SelfTest,
  [string]$PolicyPath,
  [string]$HardwarePolicyPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
[Environment]::CurrentDirectory = $repoRoot
. "$PSScriptRoot\release-common.ps1"

if (-not $PolicyPath) { $PolicyPath = Join-Path $PSScriptRoot 'release-policy.json' }
if (-not $HardwarePolicyPath) { $HardwarePolicyPath = Join-Path $PSScriptRoot 'hardware-acceptance-policy.json' }

# ===========================================================================
# Self-test: isolated fixture proving accept-then-reject behaviour.
# ===========================================================================
function New-CompleteHardwareFixture {
  # Build, under $Root: a minimal valid release package + a COMPLETE passing
  # hardware-results.json + non-empty evidence, driven by a small fixture policy
  # whose single item uses the fixture's default image. Returns a context object.
  param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Commit)
  $releaseDir = Join-Path $Root 'release-pkg'
  $info = New-MinimalReleasePackage -Dir $releaseDir -Commit $Commit
  $manifest = Read-JsonFile (Join-Path $releaseDir 'manifest.json')
  $maps = Get-ManifestImageMaps $manifest
  $defaultRel = $info.DefaultRel

  # Fixture hardware policy: two default-release items (numeric + string gate).
  $hwPolicy = [ordered]@{
    schema = 'plane-radar/hardware-acceptance-policy'; schema_version = 1
    gates = [ordered]@{ 'default-release' = [ordered]@{ title = 'fixture'; soak_hours = 1 } }
    items = @(
      [ordered]@{
        id = 'fx-heap'; gate = 'default-release'; order = 1; title = 'fixture heap'; mandatory = $true
        environment = 'supermini'; image = $defaultRel
        thresholds = @([ordered]@{ key = 'min_free_heap_bytes'; op = '>='; value = 40000; unit = 'bytes'; rationale = 'x' })
        evidence = @([ordered]@{ name = 'diag/heap.txt'; description = 'heap' })
      },
      [ordered]@{
        id = 'fx-flash'; gate = 'default-release'; order = 2; title = 'fixture flash'; mandatory = $true
        environment = 'supermini'; image = $defaultRel
        thresholds = @([ordered]@{ key = 'verify_flash'; op = '=='; value = 'pass'; unit = ''; rationale = 'x' })
        evidence = @([ordered]@{ name = 'flash/verify.log'; description = 'verify' })
      }
    )
  }
  $hwPolicyPath = Join-Path $Root 'hw-policy.json'
  Write-JsonFileLf $hwPolicyPath $hwPolicy
  $hwPolicyObj = Read-JsonFile $hwPolicyPath

  # Evidence dir + non-empty evidence files.
  $evidenceDir = Join-Path $Root 'evidence'
  New-Item -ItemType Directory -Path (Join-Path $evidenceDir 'diag') -Force | Out-Null
  New-Item -ItemType Directory -Path (Join-Path $evidenceDir 'flash') -Force | Out-Null
  Write-TextFileLf (Join-Path $evidenceDir 'diag/heap.txt') "min_free_heap=51000"
  Write-TextFileLf (Join-Path $evidenceDir 'flash/verify.log') "verify_flash: OK"

  # COMPLETE passing results: start from the scaffold, then fill in pass values.
  $results = New-HardwareResultsScaffold -Policy $hwPolicyObj -Commit $Commit -DefaultRel $defaultRel `
    -DefaultEnv 'supermini' -ImageSha $maps.Sha -ImageSize $maps.Size -GeneratedUtc '2026-01-01T00:00:00Z'
  foreach ($item in $results.items) {
    $item.status = 'pass'
    $item.operator.name = 'selftest'; $item.operator.date = '2026-01-01'
    foreach ($t in $item.thresholds) {
      if ($t.key -eq 'min_free_heap_bytes') { $t.measured = 51000 }
      elseif ($t.key -eq 'verify_flash') { $t.measured = 'pass' }
    }
    foreach ($e in $item.evidence) { $e.present = $true }
  }
  $resultsPath = Join-Path $evidenceDir 'hardware-results.json'
  Write-JsonFileLf $resultsPath $results

  return [pscustomobject]@{
    EvidenceDir = $evidenceDir; ResultsPath = $resultsPath; Policy = $hwPolicyObj
    ImageSha = $maps.Sha; ImageSize = $maps.Size; Commit = $Commit; DefaultRel = $defaultRel
  }
}

function Invoke-HardwareSelfTest {
  Write-Host "=== verify-hardware-evidence self-test (isolated temp fixture) ===" -ForegroundColor Cyan
  $fixtureRoot = Join-Path $repoRoot (".hwverify-selftest-" + [guid]::NewGuid().ToString('N'))
  $commit = 'b2c3d4e5f60718293a4b5c6d7e8f901234567890'
  $script:hwPass = 0; $script:hwFail = 0
  function Expect([string]$label, [bool]$cond) {
    if ($cond) { $script:hwPass++; Write-Host "  [ok] $label" -ForegroundColor Green }
    else { $script:hwFail++; Write-Host "  [FAIL] $label" -ForegroundColor Red }
  }
  function Run-Hw($ctx) {
    return Test-HardwareEvidence -EvidenceDir $ctx.EvidenceDir -ResultsPath $ctx.ResultsPath -Policy $ctx.Policy `
      -ImageSha $ctx.ImageSha -ImageSize $ctx.ImageSize -ExpectedCommit $ctx.Commit -ExpectedEnv 'supermini' `
      -ExpectedDefaultRel $ctx.DefaultRel -Gate 'default-release' -Quiet
  }
  try {
    $base = Join-Path $fixtureRoot 'base'
    $ctx = New-CompleteHardwareFixture -Root $base -Commit $commit
    $r = Run-Hw $ctx
    Expect "complete passing record validates" ($r.Ok)
    if (-not $r.Ok) { foreach ($e in $r.Errors) { Write-Host "      $e" -ForegroundColor DarkYellow } }

    function New-TamperCtx([string]$name, [scriptblock]$mutate) {
      $dir = Join-Path $fixtureRoot $name
      Copy-Item -LiteralPath $base -Destination $dir -Recurse -Force
      $ctx2 = [pscustomobject]@{
        EvidenceDir = Join-Path $dir 'evidence'; ResultsPath = Join-Path $dir 'evidence\hardware-results.json'
        Policy = $ctx.Policy; ImageSha = $ctx.ImageSha; ImageSize = $ctx.ImageSize; Commit = $ctx.Commit; DefaultRel = $ctx.DefaultRel
      }
      & $mutate $ctx2.ResultsPath (Join-Path $dir 'evidence')
      return $ctx2
    }

    # 1. Wrong firmware SHA in the binding.
    $t = New-TamperCtx 'wrong-sha' {
      param($rp, $ev)
      $o = Get-Content -Raw $rp | ConvertFrom-Json
      $o.binding.image_sha256 = ('f' * 64)
      Write-JsonFileLf $rp $o
    }
    Expect "rejects wrong firmware SHA" (-not (Run-Hw $t).Ok)

    # 2. Missing evidence file.
    $t = New-TamperCtx 'missing-evidence' {
      param($rp, $ev)
      Remove-Item -LiteralPath (Join-Path $ev 'diag\heap.txt') -Force
    }
    Expect "rejects missing evidence" (-not (Run-Hw $t).Ok)

    # 3. Pending status.
    $t = New-TamperCtx 'pending' {
      param($rp, $ev)
      $o = Get-Content -Raw $rp | ConvertFrom-Json
      $o.items[0].status = 'pending'
      Write-JsonFileLf $rp $o
    }
    Expect "rejects pending status" (-not (Run-Hw $t).Ok)

    # 4. Numeric threshold failure (measured below the >= floor).
    $t = New-TamperCtx 'threshold-fail' {
      param($rp, $ev)
      $o = Get-Content -Raw $rp | ConvertFrom-Json
      foreach ($th in $o.items[0].thresholds) { if ($th.key -eq 'min_free_heap_bytes') { $th.measured = 100 } }
      Write-JsonFileLf $rp $o
    }
    Expect "rejects numeric threshold failure" (-not (Run-Hw $t).Ok)

    # 5. Path traversal / absolute evidence filename.
    $t = New-TamperCtx 'traversal' {
      param($rp, $ev)
      $o = Get-Content -Raw $rp | ConvertFrom-Json
      $o.items[0].evidence[0].name = '../../evil.txt'
      Write-JsonFileLf $rp $o
    }
    Expect "rejects traversal evidence filename" (-not (Run-Hw $t).Ok)

    # 6. Empty evidence file (present but zero bytes).
    $t = New-TamperCtx 'empty-evidence' {
      param($rp, $ev)
      Set-Content -LiteralPath (Join-Path $ev 'diag\heap.txt') -Value $null -NoNewline
    }
    Expect "rejects empty evidence file" (-not (Run-Hw $t).Ok)
  }
  finally {
    if (Test-Path $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
  }

  Write-Host ""
  Write-Host "Self-test: $($script:hwPass) passed, $($script:hwFail) failed." -ForegroundColor $(if ($script:hwFail -eq 0) { 'Green' } else { 'Red' })
  if ($script:hwFail -ne 0) { throw "verify-hardware-evidence self-test FAILED ($($script:hwFail))." }
  Write-Host "verify-hardware-evidence self-test PASSED." -ForegroundColor Green
}

# ===========================================================================
# Entry point.
# ===========================================================================
if ($SelfTest) { Invoke-HardwareSelfTest; return }

if (-not $Path) { throw "Specify -Path <release/<sha>> to verify, or -SelfTest." }
$releaseRoot = Get-FullPathSafe (Join-Path $repoRoot 'release')
$pkg = $Path
if (-not [System.IO.Path]::IsPathRooted($pkg)) { $pkg = Join-Path $repoRoot $Path }
$pkg = Get-FullPathSafe $pkg

# -- 1. Re-verify the release package ---------------------------------------
$git = Get-RepoGitState $repoRoot
Write-Host "=== Re-verifying release $pkg ===" -ForegroundColor Cyan
$verify = Test-ReleasePackage -PackagePath $pkg -PolicyPath $PolicyPath -ReleaseRoot $releaseRoot `
  -RequireUnderReleaseRoot $true -RequireHeadMatch (-not $AllowStaleHead) -ExpectedHeadCommit $git.Commit -Quiet
if (-not $verify.Ok) {
  Write-Host "Release verification FAILED; refusing to verify hardware evidence:" -ForegroundColor Red
  foreach ($e in $verify.Errors) { Write-Host "  - $e" -ForegroundColor Red }
  throw "Input release did not verify."
}
Write-Host "  release verified ($($verify.Checks.Count) checks)."

# -- 2. Load manifest bindings + policies -----------------------------------
$manifest = Read-JsonFile (Join-Path $pkg 'manifest.json')
$policy = Read-JsonFile $PolicyPath
$hwPolicy = Read-JsonFile $HardwarePolicyPath
$maps = Get-ManifestImageMaps $manifest
$defaultRel = $policy.default_artifact.relative_path
$defaultEnv = $policy.default_artifact.environment
$commit = [string]$manifest.git.commit

$evidenceDir = Join-Path $pkg 'hardware-evidence'
$resultsPath = Join-Path $evidenceDir 'hardware-results.json'
if (-not (Test-Path $resultsPath)) {
  throw "No hardware-results.json under $evidenceDir. Run scripts/initialize-hardware-evidence.ps1 first."
}

# -- 3. Evaluate the selected gate ------------------------------------------
Write-Host "=== Hardware acceptance ($Gate) ===" -ForegroundColor Cyan
$result = Test-HardwareEvidence -EvidenceDir $evidenceDir -ResultsPath $resultsPath -Policy $hwPolicy `
  -ImageSha $maps.Sha -ImageSize $maps.Size -ExpectedCommit $commit -ExpectedEnv $defaultEnv `
  -ExpectedDefaultRel $defaultRel -Gate $Gate

Write-Host ""
if ($result.Ok) {
  Write-Host "HARDWARE ACCEPTANCE ($Gate) PASSED: $([IO.Path]::GetFileName($pkg))" -ForegroundColor Green
  Write-Host "  $($result.Checks.Count) checks passed."
  if ($Gate -eq 'default-release') {
    Write-Host "  NOTE: this reports DEFAULT RELEASE readiness only. Worker promotion is a separate -Gate worker-promotion evaluation and is never automatic."
  } elseif ($Gate -eq 'worker-promotion') {
    Write-Host "  NOTE: worker-promotion PASS only REPORTS worker readiness. The default release artifact remains supermini; promotion is a human decision."
  }
} else {
  Write-Host "HARDWARE ACCEPTANCE ($Gate) FAILED:" -ForegroundColor Red
  foreach ($e in $result.Errors) { Write-Host "  - $e" -ForegroundColor Red }
  throw "verify-hardware-evidence failed with $($result.Errors.Count) error(s)."
}
