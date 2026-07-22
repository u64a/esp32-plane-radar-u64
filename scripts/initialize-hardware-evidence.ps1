<#
.SYNOPSIS
  Create a PENDING hardware acceptance evidence template bound to a verified
  local release, for a human operator to complete on real hardware.

.DESCRIPTION
  1. Re-verifies the input release package (calls the same verification core as
     verify-release.ps1) -- refuses to proceed unless it passes.
  2. Reads the release manifest to bind to the EXACT default merged image
     (supermini/firmware-merged.bin): commit, environment, relative path, byte
     length, and full SHA-256; and to bind each item to its own flashed image.
  3. Writes <release>/hardware-evidence/hardware-results.json with every policy
     item status='pending', numeric thresholds (measured=null), exact expected
     evidence filenames, and empty operator fields; and creates the (empty)
     evidence subdirectories. It NEVER fabricates passing evidence.

  The generated evidence lives under the ignored release package and is not
  committed. Refuses to overwrite an existing hardware-results.json unless -Force.

.PARAMETER Path
  The verified release package directory (release/<sha>), absolute or repo-relative.

.PARAMETER Force
  Replace an existing hardware-results.json (evidence files are preserved).

.PARAMETER AllowStaleHead
  Permit a release whose commit does not match the current HEAD (archived package).
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory, Position = 0)]
  [string]$Path,
  [switch]$Force,
  [switch]$AllowStaleHead
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
[Environment]::CurrentDirectory = $repoRoot
. "$PSScriptRoot\release-common.ps1"

# The release + hardware acceptance policies are ALWAYS the tracked git-blob files
# at the package commit (loaded below). There is NO public working-tree override.

# -- Resolve + re-verify the release package --------------------------------
$releaseRoot = Get-FullPathSafe (Join-Path $repoRoot 'release')
$pkg = $Path
if (-not [System.IO.Path]::IsPathRooted($pkg)) { $pkg = Join-Path $repoRoot $Path }
$pkg = Get-FullPathSafe $pkg

# Reparse-point defence for the release root and the chain down to the package.
$rootReparse = Find-ReparsePointInChain -Base $repoRoot -Full $pkg
if ($rootReparse) { throw "Refusing to initialize: a symlink/junction/reparse point is present on the path chain: $rootReparse" }

$git = Get-RepoGitState $repoRoot
$pkgCommit = Split-Path $pkg -Leaf
if (-not (Test-Sha1Hex $pkgCommit)) { throw "Package directory leaf '$pkgCommit' is not a 40-hex commit; cannot bind tracked policy." }
$gp = Get-GitPolicy -RepoRoot $repoRoot -Commit $pkgCommit -RelPath 'scripts/release-policy.json'
$ghw = Get-GitPolicy -RepoRoot $repoRoot -Commit $pkgCommit -RelPath 'scripts/hardware-acceptance-policy.json'
$policy = $gp.Object
$hwPolicy = $ghw.Object

Write-Host "=== Re-verifying release $pkg ===" -ForegroundColor Cyan
$verify = Test-ReleasePackage -PackagePath $pkg -Policy $policy -PolicySha256 $gp.Sha256 -ReleaseRoot $releaseRoot `
  -RequireUnderReleaseRoot $true -RequireHeadMatch (-not $AllowStaleHead) -ExpectedHeadCommit $git.Commit -Quiet
if (-not $verify.Ok) {
  Write-Host "Release verification FAILED; refusing to initialize hardware evidence:" -ForegroundColor Red
  foreach ($e in $verify.Errors) { Write-Host "  - $e" -ForegroundColor Red }
  throw "Input release did not verify."
}
Write-Host "  release verified ($($verify.Checks.Count) checks)."

# -- Bind to the exact default image ----------------------------------------
$manifest = Read-JsonFile (Join-Path $pkg 'manifest.json')
$maps = Get-ManifestImageMaps $manifest
$defaultRel = $policy.default_artifact.relative_path
$defaultEnv = $policy.default_artifact.environment
$commit = [string]$manifest.git.commit
if (-not $maps.Sha.ContainsKey($defaultRel)) { throw "default image '$defaultRel' not found in manifest artifacts." }

# Ensure every hardware item's image exists in the release (fail-closed).
foreach ($pi in $hwPolicy.items) {
  if (-not $maps.Sha.ContainsKey([string]$pi.image)) {
    throw "Hardware item '$($pi.id)' references image '$($pi.image)' which is not in the release package."
  }
}

# -- Evidence dir + refuse overwrite ----------------------------------------
$evidenceDir = Join-Path $pkg 'hardware-evidence'
if (-not (Test-PathInside -Base $pkg -Candidate $evidenceDir)) { throw "evidence dir escaped the release package." }
# Reparse-point defence: refuse a junction/symlink evidence root or reparse
# points anywhere under an existing evidence dir.
$evReparse = Find-ReparsePointInChain -Base $pkg -Full $evidenceDir
if ($evReparse) { throw "Refusing: a symlink/junction/reparse point is present on the evidence path chain: $evReparse" }
if (Test-Path -LiteralPath $evidenceDir) {
  $evUnder = @(Get-ContainedReparsePoints $evidenceDir)
  if ($evUnder.Count -gt 0) { throw ("Refusing: reparse point(s) under the evidence dir: " + [string]::Join('; ', @($evUnder | Select-Object -First 3))) }
}
$resultsPath = Join-Path $evidenceDir 'hardware-results.json'
if ((Test-Path $resultsPath) -and -not $Force) {
  throw "hardware-results.json already exists at $resultsPath. Re-run with -Force to regenerate the template (evidence files are preserved)."
}
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null

# -- Build the pending scaffold + evidence subdirectories --------------------
$generatedUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$scaffold = New-HardwareResultsScaffold -Policy $hwPolicy -Commit $commit -DefaultRel $defaultRel `
  -DefaultEnv $defaultEnv -ImageSha $maps.Sha -ImageSize $maps.Size -GeneratedUtc $generatedUtc -PolicySha256 $ghw.Sha256

# Create the (empty) evidence subdirectories referenced by the policy so the
# operator has a deterministic place to drop each file. No fake evidence files.
$evidenceSubdirs = @{}
foreach ($pi in $hwPolicy.items) {
  foreach ($e in $pi.evidence) {
    $name = [string]$e.name
    if (-not (Test-RelPathSafe $name)) { throw "policy evidence name '$name' is unsafe." }
    if ($name.Contains('/')) {
      $sub = $name.Substring(0, $name.LastIndexOf('/'))
      $evidenceSubdirs[$sub] = $true
    }
  }
}
foreach ($sub in $evidenceSubdirs.Keys) {
  $subPath = Join-Path $evidenceDir ($sub -replace '/', '\')
  if (-not (Test-PathInside -Base $evidenceDir -Candidate $subPath)) { throw "evidence subdir escaped." }
  New-Item -ItemType Directory -Path $subPath -Force | Out-Null
}

Write-JsonFileLf $resultsPath $scaffold

# A short operator README (informational, not evidence).
$readme = @"
Plane Radar -- hardware acceptance evidence (PENDING)

Release commit : $commit
Default image  : $defaultRel
Image SHA-256  : $($maps.Sha[$defaultRel])
Image size     : $($maps.Size[$defaultRel]) bytes
Generated (UTC): $generatedUtc

HOW TO USE
  1. Follow scripts/hardware-acceptance-policy.json for each item's exact
     procedure, commands, thresholds, and evidence filenames.
  2. Drop each required evidence file at the path shown in hardware-results.json
     (relative to this hardware-evidence/ directory).
  3. For each evidence entry, record the file's byte 'size' and full lowercase
     'sha256' (the verifier re-hashes the file and binds it cryptographically;
     the old editable 'present' boolean is gone).
  4. Fill in each item's threshold 'measured' values and 'operator' fields
     (name + ISO date YYYY-MM-DD are REQUIRED for a mandatory pass), and set
     'status' to 'pass' ONLY when every threshold is met with real evidence.
  5. Run:  .\scripts\verify-hardware-evidence.ps1 -Path release/$commit
     (add -Gate worker-promotion to evaluate the separate 72 h worker gate).

This template contains NO passing evidence. The DEFAULT release image is
supermini/firmware-merged.bin; the worker images are evaluation-only and are
promoted ONLY through the separate worker-promotion gate.
"@
Write-TextFileLf (Join-Path $evidenceDir 'README.txt') $readme

Write-Host ""
Write-Host "Initialized PENDING hardware evidence:" -ForegroundColor Green
Write-Host "  results : $resultsPath"
Write-Host "  bound to: $defaultRel  sha256=$($maps.Sha[$defaultRel])  size=$($maps.Size[$defaultRel])"
Write-Host "  items   : $($hwPolicy.items.Count) ($((@($hwPolicy.items | Where-Object { $_.gate -eq 'default-release' })).Count) default-release, $((@($hwPolicy.items | Where-Object { $_.gate -eq 'worker-promotion' })).Count) worker-promotion), all status=pending"
Write-Host "Complete the evidence, then run scripts/verify-hardware-evidence.ps1."
