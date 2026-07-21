<#
.SYNOPSIS
  Re-verify an existing LOCAL release package WITHOUT rebuilding.

.DESCRIPTION
  Independently re-validates a release package produced by build-release.ps1:
    * the package path is safe (strictly inside <repo>/release/);
    * manifest <-> package-directory commit binding (and, by default, that the
      commit matches the CURRENT repository HEAD; use -AllowStaleHead only to
      validate a deliberately-archived older local package);
    * every manifest artifact's size AND SHA-256;
    * every CHECKSUMS.sha256 line (recomputed), with NO missing/extra/duplicate
      paths and NO traversal/absolute paths, covering manifest + all files;
    * the binary proof is overall pass with EVERY invariant pass;
    * the default artifact identity + role (supermini/firmware-merged.bin) and the
      evaluation-only role of the worker images;
    * the approved EXACT resource/file-size policy.

  With -SelfTest it builds a synthetic package in an isolated temp fixture (NEVER
  under release/) and proves the verifier ACCEPTS a well-formed package and then
  REJECTS representative tampering: artifact byte, manifest commit, checksum,
  proof=false, path traversal, duplicate CHECKSUMS entry, and a missing entry.

.PARAMETER Path
  The release package directory to verify (absolute, or relative to the repo).

.PARAMETER AllowStaleHead
  Permit a package whose commit does not match the current HEAD (archived local
  package). The manifest<->directory commit binding is still enforced.

.PARAMETER SelfTest
  Run the isolated tamper self-test instead of verifying a real package.

.PARAMETER PolicyPath
  Override the approved policy file (default: scripts/release-policy.json).
#>
[CmdletBinding(DefaultParameterSetName = 'Verify')]
param(
  [Parameter(ParameterSetName = 'Verify', Position = 0)]
  [string]$Path,
  [Parameter(ParameterSetName = 'Verify')]
  [switch]$AllowStaleHead,
  [Parameter(ParameterSetName = 'SelfTest')]
  [switch]$SelfTest,
  [string]$PolicyPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
[Environment]::CurrentDirectory = $repoRoot
. "$PSScriptRoot\release-common.ps1"

if (-not $PolicyPath) { $PolicyPath = Join-Path $PSScriptRoot 'release-policy.json' }

# ===========================================================================
# Test-ReleasePackage + New-MinimalReleasePackage are provided by
# release-common.ps1 (shared with the hardware evidence tooling).
# ===========================================================================

# ===========================================================================
# Self-test: isolated temp fixture + tamper matrix.
# ===========================================================================
function New-VerifierFixture {
  param([Parameter(Mandatory)][string]$Dir, [Parameter(Mandatory)][string]$Commit)
  $info = New-MinimalReleasePackage -Dir $Dir -Commit $Commit
  return $info.PolicyPath
}

function Invoke-VerifierSelfTest {
  Write-Host "=== verify-release self-test (isolated temp fixture) ===" -ForegroundColor Cyan
  $fixtureRoot = Join-Path $repoRoot (".relverify-selftest-" + [guid]::NewGuid().ToString('N'))
  $commit = 'a1b2c3d4e5f60718293a4b5c6d7e8f9012345678'
  $script:selfTestPass = 0; $script:selfTestFail = 0
  function Expect([string]$label, [bool]$condition) {
    if ($condition) { $script:selfTestPass++; Write-Host "  [ok] $label" -ForegroundColor Green }
    else { $script:selfTestFail++; Write-Host "  [FAIL] $label" -ForegroundColor Red }
  }
  function Run-Verify([string]$dir, [string]$policyPath) {
    return Test-ReleasePackage -PackagePath $dir -PolicyPath $policyPath -RequireUnderReleaseRoot $false -RequireHeadMatch $true -ExpectedHeadCommit $commit -Quiet
  }
  try {
    # -- Baseline: a well-formed fixture MUST verify. --------------------------
    $base = Join-Path $fixtureRoot 'base'
    $polPath = New-VerifierFixture -Dir $base -Commit $commit
    $r = Run-Verify $base $polPath
    Expect "well-formed package verifies" ($r.Ok)
    if (-not $r.Ok) { foreach ($e in $r.Errors) { Write-Host "      $e" -ForegroundColor DarkYellow } }

    # Helper: fresh tampered copy of the fixture.
    function New-Tampered([string]$name, [scriptblock]$mutate) {
      $dir = Join-Path $fixtureRoot $name
      Copy-Item -LiteralPath $base -Destination $dir -Recurse -Force
      & $mutate $dir
      return $dir
    }

    # 1. Artifact byte flip (hash mismatch), CHECKSUMS/manifest NOT updated.
    $d = New-Tampered 'tamper-artifact' {
      param($dir)
      $p = Join-Path $dir 'supermini\firmware.bin'
      $b = [System.IO.File]::ReadAllBytes($p); $b[0] = [byte](($b[0] + 1) % 256)
      [System.IO.File]::WriteAllBytes($p, $b)
    }
    Expect "rejects tampered artifact byte" (-not (Run-Verify $d $polPath).Ok)

    # 2. Manifest commit changed (binding broken).
    $d = New-Tampered 'tamper-commit' {
      param($dir)
      $mp = Join-Path $dir 'manifest.json'
      $t = Get-Content -Raw $mp
      $t = $t -replace [regex]::Escape($commit), 'deadbeefdeadbeefdeadbeefdeadbeefdeadbeef'
      Write-TextFileLf $mp $t
    }
    Expect "rejects altered manifest commit" (-not (Run-Verify $d $polPath).Ok)

    # 3. Corrupted CHECKSUMS hash.
    $d = New-Tampered 'tamper-checksum' {
      param($dir)
      $cp = Join-Path $dir 'CHECKSUMS.sha256'
      $lines = Get-Content $cp
      $lines[0] = ('0' * 64) + '  ' + ($lines[0] -replace '^[0-9a-f]{64}  ', '')
      Write-TextFileLf $cp ($lines -join "`n")
    }
    Expect "rejects corrupted CHECKSUMS hash" (-not (Run-Verify $d $polPath).Ok)

    # 4. Binary proof forced to false (invariant fail).
    $d = New-Tampered 'tamper-proof' {
      param($dir)
      $pp = Join-Path $dir 'binary-proof.json'
      $obj = Get-Content -Raw $pp | ConvertFrom-Json
      $obj.overall = 'fail'; $obj.invariants[0].status = 'fail'
      Write-JsonFileLf $pp $obj
      # Re-checksum so the ONLY failure is the proof content itself.
      Write-PackageChecksums -PackageDir $dir
      $mp = Join-Path $dir 'manifest.json'; $man = Get-Content -Raw $mp | ConvertFrom-Json
      $art = New-PackageArtifactsMap -PackageDir $dir -ExcludeRel @('manifest.json', 'CHECKSUMS.sha256')
      $man.artifacts = $art; Write-JsonFileLf $mp $man; Write-PackageChecksums -PackageDir $dir
    }
    Expect "rejects proof=false (all else consistent)" (-not (Run-Verify $d $polPath).Ok)

    # 5. Path traversal entry appended to CHECKSUMS.
    $d = New-Tampered 'tamper-traversal' {
      param($dir)
      $cp = Join-Path $dir 'CHECKSUMS.sha256'
      Add-Content -LiteralPath $cp -Value (("{0}  {1}" -f ('a' * 64), '../evil.bin'))
    }
    Expect "rejects traversal CHECKSUMS path" (-not (Run-Verify $d $polPath).Ok)

    # 6. Duplicate CHECKSUMS entry.
    $d = New-Tampered 'tamper-duplicate' {
      param($dir)
      $cp = Join-Path $dir 'CHECKSUMS.sha256'
      $lines = @(Get-Content $cp)
      Write-TextFileLf $cp (($lines + $lines[0]) -join "`n")
    }
    Expect "rejects duplicate CHECKSUMS entry" (-not (Run-Verify $d $polPath).Ok)

    # 7. Missing CHECKSUMS entry (drop coverage for a present file).
    $d = New-Tampered 'tamper-missing' {
      param($dir)
      $cp = Join-Path $dir 'CHECKSUMS.sha256'
      $lines = @(Get-Content $cp | Where-Object { $_ -notmatch 'supermini/nm-symbols\.txt$' })
      Write-TextFileLf $cp ($lines -join "`n")
    }
    Expect "rejects missing CHECKSUMS coverage" (-not (Run-Verify $d $polPath).Ok)

    # 8. Manifest resource altered (policy mismatch).
    $d = New-Tampered 'tamper-resource' {
      param($dir)
      $mp = Join-Path $dir 'manifest.json'; $man = Get-Content -Raw $mp | ConvertFrom-Json
      $man.environments[0].resources.merged_bin = 999999
      Write-JsonFileLf $mp $man
      $art = New-PackageArtifactsMap -PackageDir $dir -ExcludeRel @('manifest.json', 'CHECKSUMS.sha256')
      $man2 = Get-Content -Raw $mp | ConvertFrom-Json; $man2.artifacts = $art
      Write-JsonFileLf $mp $man2; Write-PackageChecksums -PackageDir $dir
    }
    Expect "rejects manifest resource != policy" (-not (Run-Verify $d $polPath).Ok)
  }
  finally {
    if (Test-Path $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
  }

  Write-Host ""
  Write-Host "Self-test: $($script:selfTestPass) passed, $($script:selfTestFail) failed." -ForegroundColor $(if ($script:selfTestFail -eq 0) { 'Green' } else { 'Red' })
  if ($script:selfTestFail -ne 0) { throw "verify-release self-test FAILED ($($script:selfTestFail))." }
  Write-Host "verify-release self-test PASSED." -ForegroundColor Green
}

# ===========================================================================
# Entry point.
# ===========================================================================
if ($SelfTest) {
  Invoke-VerifierSelfTest
  return
}

if (-not $Path) { throw "Specify -Path <release/<sha>> to verify, or -SelfTest." }
$releaseRoot = Get-FullPathSafe (Join-Path $repoRoot 'release')
$pkg = $Path
if (-not [System.IO.Path]::IsPathRooted($pkg)) { $pkg = Join-Path $repoRoot $Path }
$pkg = Get-FullPathSafe $pkg

$git = Get-RepoGitState $repoRoot
Write-Host "=== Verifying $pkg ===" -ForegroundColor Cyan
$result = Test-ReleasePackage -PackagePath $pkg -PolicyPath $PolicyPath -ReleaseRoot $releaseRoot `
  -RequireUnderReleaseRoot $true -RequireHeadMatch (-not $AllowStaleHead) -ExpectedHeadCommit $git.Commit

Write-Host ""
if ($result.Ok) {
  Write-Host "RELEASE PACKAGE VERIFIED: $([IO.Path]::GetFileName($pkg))" -ForegroundColor Green
  Write-Host "  $($result.Checks.Count) checks passed."
} else {
  Write-Host "RELEASE PACKAGE VERIFICATION FAILED:" -ForegroundColor Red
  foreach ($e in $result.Errors) { Write-Host "  - $e" -ForegroundColor Red }
  throw "verify-release failed with $($result.Errors.Count) error(s)."
}
