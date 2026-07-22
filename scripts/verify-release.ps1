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
  $proofStub = { param($pkg, $pol) Get-FixtureProofInvariants -EnvName 'supermini' }
  function Run-Verify([string]$dir, [string]$policyPath) {
    return Test-ReleasePackage -PackagePath $dir -PolicyPath $policyPath -RequireUnderReleaseRoot $false -RequireHeadMatch $true -ExpectedHeadCommit $commit -ProofRecomputer $proofStub -Quiet
  }
  # Re-write CHECKSUMS after a manifest/proof edit so the intended NEW check is the
  # one that fails (not merely a checksum mismatch).
  function Rechecksum([string]$dir) { Write-PackageChecksums -PackageDir $dir }
  function Edit-Manifest([string]$dir, [scriptblock]$mut) {
    $mp = Join-Path $dir 'manifest.json'; $man = Get-Content -Raw $mp | ConvertFrom-Json
    & $mut $man; Write-JsonFileLf $mp $man; Rechecksum $dir
  }
  function Edit-Proof([string]$dir, [scriptblock]$mut) {
    $pp = Join-Path $dir 'binary-proof.json'; $obj = Get-Content -Raw $pp | ConvertFrom-Json
    & $mut $obj; Write-JsonFileLf $pp $obj; Rechecksum $dir
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

    # ---- Certification tamper matrix (Phase 12 fail-closed) -----------------

    # 9. Non-certified build (build.certified=false).
    $d = New-Tampered 'tamper-uncertified' { param($dir) Edit-Manifest $dir { param($m) $m.build.certified = $false } }
    Expect "rejects build.certified=false" (-not (Run-Verify $d $polPath).Ok)

    # 10. clean_build=false.
    $d = New-Tampered 'tamper-dirty-build' { param($dir) Edit-Manifest $dir { param($m) $m.build.clean_build = $false } }
    Expect "rejects build.clean_build=false" (-not (Run-Verify $d $polPath).Ok)

    # 11. Dirty tracked worktree.
    $d = New-Tampered 'tamper-tracked-dirty' { param($dir) Edit-Manifest $dir { param($m) $m.git.tracked_clean = $false } }
    Expect "rejects git.tracked_clean=false" (-not (Run-Verify $d $polPath).Ok)

    # 12. Untracked files present.
    $d = New-Tampered 'tamper-untracked' { param($dir) Edit-Manifest $dir { param($m) $m.git.untracked_files = 3 } }
    Expect "rejects git.untracked_files>0" (-not (Run-Verify $d $polPath).Ok)

    # 13. Remote configured (not local-only).
    $d = New-Tampered 'tamper-remote' { param($dir) Edit-Manifest $dir { param($m) $m.local_only.no_remote = $false } }
    Expect "rejects local_only.no_remote=false" (-not (Run-Verify $d $polPath).Ok)

    # 14. Gate status skipped.
    $d = New-Tampered 'tamper-gate-skipped' { param($dir) Edit-Manifest $dir { param($m) $m.gates[0].status = 'skipped' } }
    Expect "rejects a skipped source gate" (-not (Run-Verify $d $polPath).Ok)

    # 15. Missing gate (drop one from the required set).
    $d = New-Tampered 'tamper-gate-missing' { param($dir) Edit-Manifest $dir { param($m) $m.gates = @($m.gates[0]) } }
    Expect "rejects a missing source gate" (-not (Run-Verify $d $polPath).Ok)

    # 16. Duplicate gate.
    $d = New-Tampered 'tamper-gate-duplicate' { param($dir) Edit-Manifest $dir { param($m) $m.gates = @($m.gates[0], $m.gates[0], $m.gates[1]) } }
    Expect "rejects a duplicate source gate" (-not (Run-Verify $d $polPath).Ok)

    # 17. gates_run=false.
    $d = New-Tampered 'tamper-gates-not-run' { param($dir) Edit-Manifest $dir { param($m) $m.tests.gates_run = $false } }
    Expect "rejects tests.gates_run=false" (-not (Run-Verify $d $polPath).Ok)

    # 18. native-test summary inconsistent with policy.
    $d = New-Tampered 'tamper-native-summary' { param($dir) Edit-Manifest $dir { param($m) $m.tests.native_test = 'tampered summary' } }
    Expect "rejects inconsistent native-test summary" (-not (Run-Verify $d $polPath).Ok)

    # 19. Expected pin mismatch (framework version altered).
    $d = New-Tampered 'tamper-pin-expected' { param($dir) Edit-Manifest $dir { param($m) $m.pins.framework_arduinoespressif32 = 'WRONG' } }
    Expect "rejects expected pin mismatch" (-not (Run-Verify $d $polPath).Ok)

    # 20. Observed pin mismatch (installed toolchain version altered).
    $d = New-Tampered 'tamper-pin-observed' { param($dir) Edit-Manifest $dir { param($m) $m.pins.observed.tool_esptoolpy = 'WRONG' } }
    Expect "rejects observed pin mismatch" (-not (Run-Verify $d $polPath).Ok)

    # 21. Environment option mismatch (log_level flipped).
    $d = New-Tampered 'tamper-env-option' { param($dir) Edit-Manifest $dir { param($m) $m.environments[0].options.log_level = 0 } }
    Expect "rejects env option mismatch" (-not (Run-Verify $d $polPath).Ok)

    # 22. Default artifact size mismatch (size no longer binds artifact entry).
    $d = New-Tampered 'tamper-default-size' { param($dir) Edit-Manifest $dir { param($m) $m.default_artifact.size = 999999 } }
    Expect "rejects default artifact size mismatch" (-not (Run-Verify $d $polPath).Ok)

    # 23. Unlisted extra file, CHECKSUMS RECOMPUTED to cover it (manifest NOT updated).
    $d = New-Tampered 'tamper-extra-checksummed' {
      param($dir)
      [System.IO.File]::WriteAllBytes((Join-Path $dir 'supermini\rogue.bin'), [byte[]](1..16))
      Rechecksum $dir   # covers the rogue file, but it is still not a manifest artifact
    }
    Expect "rejects unlisted extra file (even re-checksummed)" (-not (Run-Verify $d $polPath).Ok)

    # 24. Extra file fully wired into artifacts + CHECKSUMS (wrong copied-artifact name).
    $d = New-Tampered 'tamper-extra-wired' {
      param($dir)
      [System.IO.File]::WriteAllBytes((Join-Path $dir 'supermini\rogue.bin'), [byte[]](1..16))
      $art = New-PackageArtifactsMap -PackageDir $dir -ExcludeRel @('manifest.json', 'CHECKSUMS.sha256')
      Edit-Manifest $dir { param($m) $m.artifacts = $art }
    }
    Expect "rejects wired extra file (unexpected artifact name)" (-not (Run-Verify $d $polPath).Ok)

    # 25. Manifest-listed artifact whose file is missing.
    $d = New-Tampered 'tamper-artifact-missing-file' {
      param($dir)
      Remove-Item -LiteralPath (Join-Path $dir 'supermini\firmware.map') -Force
      Rechecksum $dir
    }
    Expect "rejects manifest artifact with missing file" (-not (Run-Verify $d $polPath).Ok)

    # 26. Forged proof: an invariant status flipped to 'fail' (overall left 'pass').
    $d = New-Tampered 'tamper-proof-forged-status' { param($dir) Edit-Proof $dir { param($p) $p.invariants[0].status = 'fail' } }
    Expect "rejects forged invariant status" (-not (Run-Verify $d $polPath).Ok)

    # 27. Forged proof: an invariant observed value altered (contradicts re-derivation).
    $d = New-Tampered 'tamper-proof-forged-observed' { param($dir) Edit-Proof $dir { param($p) $p.invariants[0].observed = 'tampered' } }
    Expect "rejects forged invariant observed" (-not (Run-Verify $d $polPath).Ok)

    # 28. Trimmed proof: an invariant removed and totals lowered to match.
    $d = New-Tampered 'tamper-proof-trimmed' {
      param($dir)
      Edit-Proof $dir { param($p) $p.invariants = @($p.invariants[0]); $p.total = 1; $p.passed = 1 }
    }
    Expect "rejects trimmed proof invariant set" (-not (Run-Verify $d $polPath).Ok)

    # 29. Contradictory proof totals (overall 'pass' but declared failed>0).
    $d = New-Tampered 'tamper-proof-contradictory' {
      param($dir)
      Edit-Proof $dir { param($p) $p.passed = 1; $p.failed = 1 }
    }
    Expect "rejects contradictory proof totals" (-not (Run-Verify $d $polPath).Ok)

    # 30. ADS / colon path smuggled into CHECKSUMS.
    $d = New-Tampered 'tamper-ads-checksum' {
      param($dir)
      $cp = Join-Path $dir 'CHECKSUMS.sha256'
      Add-Content -LiteralPath $cp -Value (("{0}  {1}" -f ('a' * 64), 'supermini/firmware.bin:evil'))
    }
    Expect "rejects ADS/colon CHECKSUMS path" (-not (Run-Verify $d $polPath).Ok)
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
