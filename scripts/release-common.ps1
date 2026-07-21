# =============================================================================
# scripts/release-common.ps1
#
# Shared, side-effect-free helpers for the Phase 12 LOCAL release pipeline
# (scripts/build-release.ps1 and scripts/verify-release.ps1) and the hardware
# evidence tooling. Dot-source it:  . "$PSScriptRoot\release-common.ps1"
#
# Hard requirements honoured here:
#   * Windows PowerShell 5.1 AND pwsh 7 compatible (no ternary, no null-
#     coalescing, no `if` used as an expression, no `-AsHashtable`).
#   * Deterministic file output: LF line endings, UTF-8 with NO BOM.
#   * Binary-safe byte searching (ISO-8859-1 1:1 byte<->char map, ordinal
#     IndexOf) -- never a text-decoding guess.
#   * Path-traversal safe: every published/deleted path is resolved and proven
#     to live strictly inside <repo>\release before anything is written/removed.
# This module NEVER builds, deletes .pio, publishes, or touches the network.
#
# NOTE: this module deliberately does NOT call Set-StrictMode, so dot-sourcing it
# cannot impose strict mode on the existing policy gates that build-release.ps1
# invokes in child scopes.
# =============================================================================

# ---------------------------------------------------------------------------
# Paths & general helpers
# ---------------------------------------------------------------------------

function Get-FullPathSafe {
  param([Parameter(Mandatory)][string]$Path)
  # GetFullPath collapses ..\ and . segments WITHOUT requiring existence, so it
  # is the correct primitive for traversal defence (Resolve-Path throws on
  # missing paths and would leak partial results).
  return [System.IO.Path]::GetFullPath($Path)
}

function Test-PathInside {
  [OutputType([bool])]
  param(
    [Parameter(Mandatory)][string]$Base,
    [Parameter(Mandatory)][string]$Candidate,
    [switch]$AllowEqual
  )
  $sep = [System.IO.Path]::DirectorySeparatorChar
  $b = (Get-FullPathSafe $Base).TrimEnd('\', '/')
  $c = (Get-FullPathSafe $Candidate).TrimEnd('\', '/')
  if ($AllowEqual -and $c.Equals($b, [System.StringComparison]::OrdinalIgnoreCase)) {
    return $true
  }
  return $c.StartsWith($b + $sep, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-Sha1Hex {
  [OutputType([bool])]
  param([string]$Value)
  if ($null -eq $Value) { return $false }
  return [bool]([regex]::IsMatch($Value, '^[0-9a-f]{40}$'))
}

function Test-Sha256Hex {
  [OutputType([bool])]
  param([string]$Value)
  if ($null -eq $Value) { return $false }
  return [bool]([regex]::IsMatch($Value, '^[0-9a-f]{64}$'))
}

# ---------------------------------------------------------------------------
# Hashing
# ---------------------------------------------------------------------------

function Get-Sha256Hex {
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$Path)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $stream = [System.IO.File]::OpenRead($Path)
    try { $hash = $sha.ComputeHash($stream) } finally { $stream.Dispose() }
  } finally { $sha.Dispose() }
  $sb = New-Object System.Text.StringBuilder ($hash.Length * 2)
  foreach ($b in $hash) { [void]$sb.Append($b.ToString('x2')) }
  return $sb.ToString()
}

function Get-Sha256HexOfBytes {
  [OutputType([string])]
  param([Parameter(Mandatory)][byte[]]$Bytes)
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try { $hash = $sha.ComputeHash($Bytes) } finally { $sha.Dispose() }
  $sb = New-Object System.Text.StringBuilder ($hash.Length * 2)
  foreach ($b in $hash) { [void]$sb.Append($b.ToString('x2')) }
  return $sb.ToString()
}

# ---------------------------------------------------------------------------
# Deterministic file output (LF, UTF-8 no BOM)
# ---------------------------------------------------------------------------

function ConvertTo-LfText {
  [OutputType([string])]
  param([string]$Text)
  if ($null -eq $Text) { return "" }
  return ($Text -replace "`r`n", "`n") -replace "`r", "`n"
}

function Write-TextFileLf {
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
    [switch]$NoTrailingNewline
  )
  $lf = ConvertTo-LfText $Text
  if (-not $NoTrailingNewline -and -not $lf.EndsWith("`n")) { $lf += "`n" }
  $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $lf, $utf8NoBom)
}

function ConvertTo-CanonicalJson {
  # Deterministic-shape JSON. Insertion order is preserved by feeding [ordered]
  # dictionaries; line endings are normalised to LF and a trailing newline is
  # added so the file hashes stably. -Depth is forced high because Windows
  # PowerShell 5.1 silently truncates nested objects at depth 2 by default.
  [OutputType([string])]
  param([Parameter(Mandatory)]$Object)
  $json = $Object | ConvertTo-Json -Depth 64
  return (ConvertTo-LfText $json)
}

function Write-JsonFileLf {
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)]$Object
  )
  Write-TextFileLf -Path $Path -Text (ConvertTo-CanonicalJson $Object)
}

# ---------------------------------------------------------------------------
# Git state (all read-only)
# ---------------------------------------------------------------------------

function Invoke-GitRaw {
  param(
    [Parameter(Mandatory)][string]$RepoRoot,
    [Parameter(Mandatory)][string[]]$GitArgs
  )
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & git -C $RepoRoot @GitArgs 2>$null
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $prev }
  return [pscustomobject]@{ ExitCode = $code; Output = $out }
}

function Get-RepoGitState {
  [OutputType([pscustomobject])]
  param([Parameter(Mandatory)][string]$RepoRoot)

  $head = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('rev-parse', 'HEAD')
  if ($head.ExitCode -ne 0) { throw "git HEAD not found in $RepoRoot (not a git repo, or no commits)." }
  $commit = ([string]($head.Output | Select-Object -First 1)).Trim()
  if (-not (Test-Sha1Hex $commit)) { throw "git rev-parse HEAD did not return a 40-hex commit: '$commit'." }

  $branchRes = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('rev-parse', '--abbrev-ref', 'HEAD')
  $branch = ([string]($branchRes.Output | Select-Object -First 1)).Trim()

  $remoteRes = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('remote')
  $remotes = @($remoteRes.Output | Where-Object { $_ -and $_.Trim().Length -gt 0 })

  $statusRes = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('status', '--porcelain=v1', '--untracked-files=all')
  $statusLines = @($statusRes.Output | Where-Object { $_ -ne $null -and $_.ToString().Length -gt 0 })
  $untracked = @($statusLines | Where-Object { $_.StartsWith('??') })
  $tracked = @($statusLines | Where-Object { -not $_.StartsWith('??') })

  return [pscustomobject]@{
    Commit         = $commit
    ShortCommit    = $commit.Substring(0, 7)
    Branch         = $branch
    HasRemote      = ($remotes.Count -gt 0)
    RemoteCount    = $remotes.Count
    TrackedDirty   = ($tracked.Count -gt 0)
    UntrackedCount = $untracked.Count
    StatusLines    = $statusLines
    TrackedLines   = $tracked
    UntrackedLines = $untracked
  }
}

# ---------------------------------------------------------------------------
# PlatformIO / RISC-V toolchain resolution (pinned)
# ---------------------------------------------------------------------------

function Resolve-PlatformIoCoreDir {
  [OutputType([string])]
  param()
  if ($env:PLATFORMIO_CORE_DIR -and (Test-Path $env:PLATFORMIO_CORE_DIR)) {
    return (Get-FullPathSafe $env:PLATFORMIO_CORE_DIR)
  }
  $default = Join-Path $env:USERPROFILE '.platformio'
  return (Get-FullPathSafe $default)
}

function Resolve-PlatformIoExe {
  [OutputType([string])]
  param()
  $cmd = Get-Command pio -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  $fallback = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
  if (Test-Path $fallback) { return $fallback }
  throw "PlatformIO Core (pio) not found. Install requirements-dev.txt first."
}

function Resolve-RiscvTool {
  # Resolve a pinned RISC-V binutils tool (relative path from release-policy.json,
  # e.g. toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe) under the PlatformIO
  # packages dir. Fails closed if the pinned toolchain is absent.
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$RelativePath)
  $core = Resolve-PlatformIoCoreDir
  $full = Join-Path (Join-Path $core 'packages') ($RelativePath -replace '/', '\')
  if (-not (Test-Path $full)) {
    throw "Pinned PlatformIO tool not found: $full (build the firmware envs first so the espressif32 toolchain is installed)."
  }
  return (Get-FullPathSafe $full)
}

# ---------------------------------------------------------------------------
# nm symbol evidence (demangled, size-aware)
# ---------------------------------------------------------------------------

function Get-NmLines {
  # `nm -C -S -t d <elf>`: -C demangles, -S prints symbol size, -t d makes the
  # value AND size columns decimal so the worker-stack size assertion is exact.
  [OutputType([string[]])]
  param(
    [Parameter(Mandatory)][string]$NmPath,
    [Parameter(Mandatory)][string]$ElfPath
  )
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & $NmPath -C -S -t d $ElfPath 2>$null
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $prev }
  if ($code -ne 0) { throw "nm failed (exit $code) on $ElfPath" }
  return @($out | ForEach-Object { [string]$_ })
}

function New-TokenBoundaryRegex {
  # Match an identifier as a whole token: not preceded/followed by an identifier
  # character. This is what makes `workerCancel` NOT match the pure-core symbol
  # `core::workerCancelRequested`, and `s_result_q` NOT match `s_result_queue_buf`.
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$Token)
  return ('(?<![A-Za-z0-9_])' + [regex]::Escape($Token) + '(?![A-Za-z0-9_])')
}

function Test-NmSymbolPresent {
  [OutputType([bool])]
  param(
    [Parameter(Mandatory)][string[]]$NmLines,
    [Parameter(Mandatory)][string]$Token
  )
  $rx = New-TokenBoundaryRegex $Token
  foreach ($line in $NmLines) {
    $name = Get-NmSymbolName $line
    if ($name -and [regex]::IsMatch($name, $rx)) { return $true }
  }
  return $false
}

function Get-NmSymbolName {
  # Return the demangled name portion of an nm -S -t d line, or $null.
  #   "<value> <size> <type> <name...>"  (defined, sized)
  #   "<value> <type> <name...>"          (defined, unsized)
  #   "         U <name...>"              (undefined)
  [OutputType([string])]
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Line)
  $m = [regex]::Match($Line, '^\s*(?<val>[0-9]+)?\s*(?<size>[0-9]+\s+)?(?<type>[A-Za-z?-])\s+(?<name>.+)$')
  if ($m.Success) { return $m.Groups['name'].Value }
  return $null
}

function Get-NmSymbolSizes {
  # Return the decimal sizes of every defined symbol whose name token-matches.
  [OutputType([int[]])]
  param(
    [Parameter(Mandatory)][string[]]$NmLines,
    [Parameter(Mandatory)][string]$Token
  )
  $rx = New-TokenBoundaryRegex $Token
  $sizes = New-Object System.Collections.Generic.List[int]
  foreach ($line in $NmLines) {
    $m = [regex]::Match($line, '^\s*(?<val>[0-9]+)\s+(?<size>[0-9]+)\s+(?<type>[A-Za-z])\s+(?<name>.+)$')
    if ($m.Success -and [regex]::IsMatch($m.Groups['name'].Value, $rx)) {
      $sizes.Add([int]$m.Groups['size'].Value)
    }
  }
  return , ($sizes.ToArray())
}

function Select-NmEvidenceLines {
  # Keep only nm lines whose name token-matches any of the supplied tokens --
  # a compact, auditable evidence subset saved into the release package.
  [OutputType([string[]])]
  param(
    [Parameter(Mandatory)][string[]]$NmLines,
    [Parameter(Mandatory)][string[]]$Tokens
  )
  $regexes = @($Tokens | ForEach-Object { New-TokenBoundaryRegex $_ })
  $kept = New-Object System.Collections.Generic.List[string]
  foreach ($line in $NmLines) {
    $name = Get-NmSymbolName $line
    if (-not $name) { continue }
    foreach ($rx in $regexes) {
      if ([regex]::IsMatch($name, $rx)) { $kept.Add($line); break }
    }
  }
  return , ($kept.ToArray())
}

# ---------------------------------------------------------------------------
# Binary-safe byte searching
# ---------------------------------------------------------------------------

function Get-Latin1Haystack {
  # Read a file's raw bytes and expose them as a string via the ISO-8859-1
  # (Latin-1) codepage, which maps bytes 0..255 bijectively to chars 0..255.
  # Ordinal IndexOf over this string is therefore an exact binary byte search
  # (newline 0x0A included), with none of the pitfalls of UTF-8/UTF-16 decoding.
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$Path)
  $bytes = [System.IO.File]::ReadAllBytes($Path)
  return ([System.Text.Encoding]::GetEncoding(28591)).GetString($bytes)
}

function ConvertTo-Latin1Needle {
  [OutputType([string])]
  param([Parameter(Mandatory)][byte[]]$Bytes)
  return ([System.Text.Encoding]::GetEncoding(28591)).GetString($Bytes)
}

function Get-AsciiBytesWithTrailer {
  # Build a byte needle from ASCII text plus optional trailing raw bytes (e.g.
  # the literal LF that distinguishes the `Plane Radar\n` startup log from the
  # `Plane Radar` substring baked into the portal HTML of every build).
  [OutputType([byte[]])]
  param(
    [Parameter(Mandatory)][string]$Text,
    [byte[]]$Trailer = @()
  )
  $ascii = [System.Text.Encoding]::ASCII.GetBytes($Text)
  if ($Trailer.Count -eq 0) { return $ascii }
  $out = New-Object 'System.Collections.Generic.List[byte]'
  $out.AddRange($ascii)
  $out.AddRange($Trailer)
  return , ($out.ToArray())
}

function Test-HaystackContainsBytes {
  [OutputType([bool])]
  param(
    [Parameter(Mandatory)][string]$Haystack,
    [Parameter(Mandatory)][byte[]]$Needle
  )
  $needleStr = ConvertTo-Latin1Needle $Needle
  return ($Haystack.IndexOf($needleStr, [System.StringComparison]::Ordinal) -ge 0)
}

# ---------------------------------------------------------------------------
# Invariant record helpers
# ---------------------------------------------------------------------------

function New-InvariantResult {
  [OutputType([System.Collections.Specialized.OrderedDictionary])]
  param(
    [Parameter(Mandatory)][string]$Id,
    [Parameter(Mandatory)][string]$Category,
    [string]$Environment = "",
    [Parameter(Mandatory)][string]$Description,
    [Parameter(Mandatory)][string]$Expectation,
    [Parameter(Mandatory)][string]$Observed,
    [Parameter(Mandatory)][bool]$Pass
  )
  $status = 'fail'
  if ($Pass) { $status = 'pass' }
  return [ordered]@{
    id          = $Id
    category    = $Category
    environment = $Environment
    description = $Description
    expectation = $Expectation
    observed    = $Observed
    status      = $status
  }
}

# ---------------------------------------------------------------------------
# Policy loading
# ---------------------------------------------------------------------------

function Read-JsonFile {
  [OutputType([pscustomobject])]
  param([Parameter(Mandatory)][string]$Path)
  if (-not (Test-Path $Path)) { throw "JSON file not found: $Path" }
  return (Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json)
}

function Get-PsObjectProperty {
  # Safe property access across PSCustomObject shapes (returns $null when absent)
  # without tripping Set-StrictMode.
  param([Parameter(Mandatory)]$Object, [Parameter(Mandatory)][string]$Name)
  if ($null -eq $Object) { return $null }
  $prop = $Object.PSObject.Properties[$Name]
  if ($null -eq $prop) { return $null }
  return $prop.Value
}

# ---------------------------------------------------------------------------
# Release package structure (shared by build-release and the verifier self-test)
# ---------------------------------------------------------------------------

function Get-PackageRelPath {
  # Forward-slash relative path of a file within a package root.
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$FullPath)
  $full = Get-FullPathSafe $FullPath
  $rootFull = (Get-FullPathSafe $Root).TrimEnd('\', '/')
  $rel = $full.Substring($rootFull.Length).TrimStart('\', '/')
  return ($rel -replace '\\', '/')
}

function New-PackageArtifactsMap {
  # Ordered map (ordinal by rel path) of every file under $PackageDir EXCEPT the
  # relative paths in $ExcludeRel, each -> { size, sha256, env }.
  [OutputType([System.Collections.Specialized.OrderedDictionary])]
  param(
    [Parameter(Mandatory)][string]$PackageDir,
    [string[]]$ExcludeRel = @()
  )
  $rows = New-Object System.Collections.Generic.List[object]
  foreach ($f in (Get-ChildItem -LiteralPath $PackageDir -Recurse -File)) {
    $rel = Get-PackageRelPath -Root $PackageDir -FullPath $f.FullName
    if ($ExcludeRel -contains $rel) { continue }
    $rows.Add([pscustomobject]@{ Rel = $rel; Full = $f.FullName; Size = [int64]$f.Length })
  }
  $relSorted = [string[]]@($rows | ForEach-Object { $_.Rel })
  [Array]::Sort($relSorted, [System.StringComparer]::Ordinal)
  $byRel = @{}; foreach ($r in $rows) { $byRel[$r.Rel] = $r }
  $map = [ordered]@{}
  foreach ($rel in $relSorted) {
    $r = $byRel[$rel]
    $envOwner = ''
    if ($rel.Contains('/')) { $envOwner = $rel.Split('/')[0] }
    $map[$rel] = [ordered]@{ size = [int64]$r.Size; sha256 = (Get-Sha256Hex $r.Full); env = $envOwner }
  }
  return $map
}

function Write-PackageChecksums {
  # Write CHECKSUMS.sha256 covering EVERY file under $PackageDir except CHECKSUMS
  # itself: sorted (ordinal, by forward-slash rel path) "<sha256>  <path>" lines,
  # LF / no BOM. Avoids self-reference (CHECKSUMS never hashes itself).
  param([Parameter(Mandatory)][string]$PackageDir)
  $rows = New-Object System.Collections.Generic.List[object]
  foreach ($f in (Get-ChildItem -LiteralPath $PackageDir -Recurse -File)) {
    $rel = Get-PackageRelPath -Root $PackageDir -FullPath $f.FullName
    if ($rel -eq 'CHECKSUMS.sha256') { continue }
    $rows.Add([pscustomobject]@{ Rel = $rel; Line = ("{0}  {1}" -f (Get-Sha256Hex $f.FullName), $rel) })
  }
  $relSorted = [string[]]@($rows | ForEach-Object { $_.Rel })
  [Array]::Sort($relSorted, [System.StringComparer]::Ordinal)
  $byRel = @{}; foreach ($r in $rows) { $byRel[$r.Rel] = $r.Line }
  $lines = @($relSorted | ForEach-Object { $byRel[$_] })
  Write-TextFileLf -Path (Join-Path $PackageDir 'CHECKSUMS.sha256') -Text ($lines -join "`n")
}

function Test-RelPathSafe {
  # Reject absolute paths, drive/UNC roots, and any traversal ('..') segment.
  [OutputType([bool])]
  param([Parameter(Mandatory)][AllowEmptyString()][string]$Rel)
  if ([string]::IsNullOrWhiteSpace($Rel)) { return $false }
  if ($Rel -match '^[\\/]' ) { return $false }             # leading slash (absolute)
  if ($Rel -match '^[A-Za-z]:') { return $false }          # drive-qualified
  if ($Rel -match '^[\\/][\\/]') { return $false }         # UNC
  $norm = $Rel -replace '\\', '/'
  foreach ($seg in $norm.Split('/')) {
    if ($seg -eq '..') { return $false }
  }
  return $true
}

# ---------------------------------------------------------------------------
# Release package verification core (shared by verify-release.ps1 and the
# hardware evidence tooling, which re-verifies the release before binding).
# ---------------------------------------------------------------------------

function Test-ReleasePackage {
  # Re-verify an existing release package WITHOUT rebuilding. Returns
  # [pscustomobject]@{ Ok; Errors; Checks } and never exits/throws for a normal
  # validation failure (so callers -- including self-tests -- can assert on Ok).
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$PackagePath,
    [Parameter(Mandatory)][string]$PolicyPath,
    [string]$ReleaseRoot,
    [bool]$RequireUnderReleaseRoot = $true,
    [bool]$RequireHeadMatch = $true,
    [string]$ExpectedHeadCommit,
    [switch]$Quiet
  )

  $errors = New-Object System.Collections.Generic.List[string]
  $checks = New-Object System.Collections.Generic.List[object]
  function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $status = 'FAIL'; if ($ok) { $status = 'pass' }
    $checks.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    if (-not $ok) { $errors.Add("$name -- $detail") }
    if (-not $Quiet) {
      $color = 'Green'; if (-not $ok) { $color = 'Red' }
      $suffix = ''; if ($detail) { $suffix = " -- $detail" }
      Write-Host ("  [{0}] {1}{2}" -f $status, $name, $suffix) -ForegroundColor $color
    }
  }

  $policy = Read-JsonFile $PolicyPath

  # -- 1. Path safety ---------------------------------------------------------
  $pkgFull = Get-FullPathSafe $PackagePath
  if (-not (Test-Path $pkgFull -PathType Container)) {
    Add-Check 'package-exists' $false "not a directory: $pkgFull"
    return [pscustomobject]@{ Ok = $false; Errors = $errors; Checks = $checks }
  }
  $leaf = Split-Path $pkgFull -Leaf
  if ($RequireUnderReleaseRoot) {
    $insideRelease = Test-PathInside -Base $ReleaseRoot -Candidate $pkgFull
    Add-Check 'path-under-release-root' $insideRelease "package must live strictly inside $ReleaseRoot"
    Add-Check 'path-leaf-is-sha' (Test-Sha1Hex $leaf) "package directory leaf must be a 40-hex commit (got '$leaf')"
  }

  # -- 2. Load manifest + proof ----------------------------------------------
  $manifestPath = Join-Path $pkgFull 'manifest.json'
  $checksumsPath = Join-Path $pkgFull 'CHECKSUMS.sha256'
  $proofPath = Join-Path $pkgFull 'binary-proof.json'
  foreach ($req in @($manifestPath, $checksumsPath, $proofPath)) {
    if (-not (Test-Path $req)) {
      Add-Check 'required-file' $false "missing $req"
      return [pscustomobject]@{ Ok = $false; Errors = $errors; Checks = $checks }
    }
  }
  $manifest = Read-JsonFile $manifestPath
  $proof = Read-JsonFile $proofPath

  Add-Check 'manifest-schema' (( Get-PsObjectProperty $manifest 'schema') -eq 'plane-radar/release-manifest') "unexpected manifest schema"

  # -- 3. Commit / path binding ----------------------------------------------
  $manifestCommit = [string](Get-PsObjectProperty $manifest.git 'commit')
  $boundCommit = $ExpectedHeadCommit
  if ($RequireUnderReleaseRoot) { $boundCommit = $leaf }
  Add-Check 'manifest-commit-binding' ($manifestCommit -eq $boundCommit) "manifest commit '$manifestCommit' must equal package directory '$boundCommit'"
  if ($RequireHeadMatch) {
    Add-Check 'head-match' ($manifestCommit -eq $ExpectedHeadCommit) "manifest commit '$manifestCommit' must equal current HEAD '$ExpectedHeadCommit' (use -AllowStaleHead for an archived package)"
  }
  Add-Check 'proof-commit-binding' (([string](Get-PsObjectProperty $proof 'commit')) -eq $manifestCommit) "binary-proof commit must equal manifest commit"

  # -- 4. Manifest artifact size + hash --------------------------------------
  $artifacts = Get-PsObjectProperty $manifest 'artifacts'
  $artifactProps = @($artifacts.PSObject.Properties)
  if ($artifactProps.Count -eq 0) { Add-Check 'artifacts-present' $false "manifest has no artifacts" }
  $artifactBad = 0
  foreach ($p in $artifactProps) {
    $rel = $p.Name
    if (-not (Test-RelPathSafe $rel)) { $artifactBad++; Add-Check "artifact-path/$rel" $false "unsafe artifact path"; continue }
    if ($rel -eq 'manifest.json' -or $rel -eq 'CHECKSUMS.sha256') { $artifactBad++; Add-Check "artifact-path/$rel" $false "manifest/CHECKSUMS must not be listed as artifacts"; continue }
    $file = Join-Path $pkgFull ($rel -replace '/', '\')
    if (-not (Test-PathInside -Base $pkgFull -Candidate $file)) { $artifactBad++; Add-Check "artifact-inside/$rel" $false "artifact escapes package"; continue }
    if (-not (Test-Path $file)) { $artifactBad++; Add-Check "artifact-exists/$rel" $false "missing artifact file"; continue }
    $sizeOk = ((Get-Item $file).Length -eq [int64]$p.Value.size)
    $hashOk = ((Get-Sha256Hex $file) -eq [string]$p.Value.sha256)
    if (-not ($sizeOk -and $hashOk)) { $artifactBad++ }
  }
  Add-Check 'manifest-artifacts' ($artifactBad -eq 0) "$artifactBad artifact(s) failed size/hash/path validation"

  # -- 5. CHECKSUMS: recompute, coverage, no dup/traversal/missing/extra ------
  $checksumText = Get-Content -Raw -LiteralPath $checksumsPath
  $checksumLines = @(($checksumText -replace "`r`n", "`n") -split "`n" | Where-Object { $_.Trim().Length -gt 0 })
  $covered = @{}
  $dupCount = 0; $badLine = 0; $hashMismatch = 0; $traversal = 0
  foreach ($line in $checksumLines) {
    $m = [regex]::Match($line, '^(?<hash>[0-9a-f]{64})  (?<path>.+)$')
    if (-not $m.Success) { $badLine++; continue }
    $rel = $m.Groups['path'].Value
    if (-not (Test-RelPathSafe $rel)) { $traversal++; continue }
    if ($rel -eq 'CHECKSUMS.sha256') { $badLine++; continue }  # must not self-reference
    if ($covered.ContainsKey($rel)) { $dupCount++; continue }
    $covered[$rel] = $true
    $file = Join-Path $pkgFull ($rel -replace '/', '\')
    if (-not (Test-Path $file)) { $badLine++; continue }
    if ((Get-Sha256Hex $file) -ne $m.Groups['hash'].Value) { $hashMismatch++ }
  }
  Add-Check 'checksums-parse' ($badLine -eq 0) "$badLine malformed/dangling CHECKSUMS line(s)"
  Add-Check 'checksums-no-duplicate' ($dupCount -eq 0) "$dupCount duplicate path(s)"
  Add-Check 'checksums-no-traversal' ($traversal -eq 0) "$traversal traversal/absolute path(s)"
  Add-Check 'checksums-hash-match' ($hashMismatch -eq 0) "$hashMismatch checksum hash mismatch(es)"

  # Coverage: every package file except CHECKSUMS.sha256 must be covered. The
  # operator-added hardware-evidence/ subtree is a post-build addendum with its
  # own binding + validation (verify-hardware-evidence.ps1), so it is excluded
  # from build-package coverage here.
  $actualFiles = @{}
  foreach ($f in (Get-ChildItem -LiteralPath $pkgFull -Recurse -File)) {
    $rel = Get-PackageRelPath -Root $pkgFull -FullPath $f.FullName
    if ($rel -eq 'CHECKSUMS.sha256') { continue }
    if ($rel -eq 'hardware-evidence' -or $rel -like 'hardware-evidence/*') { continue }
    $actualFiles[$rel] = $true
  }
  $missing = @($actualFiles.Keys | Where-Object { -not $covered.ContainsKey($_) })
  $extra = @($covered.Keys | Where-Object { -not $actualFiles.ContainsKey($_) })
  Add-Check 'checksums-covers-all' ($missing.Count -eq 0) "$($missing.Count) file(s) not covered: $([string]::Join(', ', @($missing | Select-Object -First 5)))"
  Add-Check 'checksums-no-extra' ($extra.Count -eq 0) "$($extra.Count) checksum entry(ies) without a file: $([string]::Join(', ', @($extra | Select-Object -First 5)))"
  Add-Check 'checksums-covers-manifest' ($covered.ContainsKey('manifest.json')) "manifest.json must be covered by CHECKSUMS"

  # -- 6. Binary proof overall + every invariant pass ------------------------
  $proofOverall = [string](Get-PsObjectProperty $proof 'overall')
  Add-Check 'proof-overall-pass' ($proofOverall -eq 'pass') "binary-proof overall is '$proofOverall'"
  $proofInvariants = @(Get-PsObjectProperty $proof 'invariants')
  $proofFail = @($proofInvariants | Where-Object { ([string](Get-PsObjectProperty $_ 'status')) -ne 'pass' })
  Add-Check 'proof-invariants-pass' ($proofFail.Count -eq 0 -and $proofInvariants.Count -gt 0) "$($proofFail.Count) of $($proofInvariants.Count) invariants not pass"
  $proofSummary = Get-PsObjectProperty $manifest 'proof_summary'
  Add-Check 'manifest-proof-summary' (([string](Get-PsObjectProperty $proofSummary 'overall')) -eq 'pass') "manifest proof_summary overall must be pass"

  # -- 7. Default artifact identity + role, worker eval-only ------------------
  $defExpected = $policy.default_artifact.relative_path
  $defManifest = Get-PsObjectProperty $manifest 'default_artifact'
  $defRel = [string](Get-PsObjectProperty $defManifest 'relative_path')
  Add-Check 'default-artifact-path' ($defRel -eq $defExpected) "default artifact must be '$defExpected' (got '$defRel')"
  Add-Check 'default-artifact-role' (([string](Get-PsObjectProperty $defManifest 'role')) -eq 'default-release') "default artifact role must be default-release"
  $defArtProp = $artifacts.PSObject.Properties[$defRel]
  if ($defArtProp) {
    Add-Check 'default-artifact-hash-binding' (([string](Get-PsObjectProperty $defManifest 'sha256')) -eq [string]$defArtProp.Value.sha256) "default_artifact sha256 must match its artifacts entry"
  } else {
    Add-Check 'default-artifact-in-artifacts' $false "default artifact '$defRel' not in artifacts map"
  }

  # -- 8. Approved exact resource policy + artifact/resource consistency ------
  $envByName = @{}
  foreach ($e in @(Get-PsObjectProperty $manifest 'environments')) { $envByName[[string](Get-PsObjectProperty $e 'name')] = $e }
  $cleanBuild = [bool](Get-PsObjectProperty (Get-PsObjectProperty $manifest 'build') 'clean_build')
  foreach ($envPolicy in $policy.environments) {
    $name = $envPolicy.name
    if (-not $envByName.ContainsKey($name)) { Add-Check "env/$name" $false "environment missing from manifest"; continue }
    $me = $envByName[$name]
    $res = Get-PsObjectProperty $me 'resources'
    $exp = $envPolicy.resources
    $rBin = [int64](Get-PsObjectProperty $res 'firmware_bin')
    $rMerged = [int64](Get-PsObjectProperty $res 'merged_bin')
    $okBin = ($rBin -eq [int64]$exp.firmware_bin)
    $okMerged = ($rMerged -eq [int64]$exp.merged_bin)
    $okRam = $true; $okFlash = $true
    $rRam = Get-PsObjectProperty $res 'ram'; $rFlash = Get-PsObjectProperty $res 'flash'
    if ($null -ne $rRam) { $okRam = ([int64]$rRam -eq [int64]$exp.ram) } elseif ($cleanBuild) { $okRam = $false }
    if ($null -ne $rFlash) { $okFlash = ([int64]$rFlash -eq [int64]$exp.flash) } elseif ($cleanBuild) { $okFlash = $false }
    $binArt = $artifacts.PSObject.Properties["$name/firmware.bin"]
    $mergedArt = $artifacts.PSObject.Properties["$name/firmware-merged.bin"]
    $consistent = ($binArt -and $mergedArt -and ([int64]$binArt.Value.size -eq $rBin) -and ([int64]$mergedArt.Value.size -eq $rMerged))
    Add-Check "resource-policy/$name" ($okBin -and $okMerged -and $okRam -and $okFlash -and $consistent) `
      "resources must equal policy (ram=$($exp.ram),flash=$($exp.flash),bin=$($exp.firmware_bin),merged=$($exp.merged_bin)) and match copied artifacts"
    $expEval = [bool]$envPolicy.evaluation_only
    Add-Check "env-role/$name" (([bool](Get-PsObjectProperty $me 'evaluation_only')) -eq $expEval) "evaluation_only must be $expEval"
  }

  return [pscustomobject]@{ Ok = ($errors.Count -eq 0); Errors = $errors; Checks = $checks }
}

function New-MinimalReleasePackage {
  # Build a SMALL but fully valid release package in $Dir for self-tests ONLY
  # (never touches real firmware, never writes under release/). Returns info
  # about the default image. Also writes a fixture policy.json into $Dir whose
  # env sizes match the fake artifacts, and returns its path.
  [OutputType([hashtable])]
  param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Commit,
    [string]$EnvName = 'supermini'
  )
  New-Item -ItemType Directory -Path $Dir -Force | Out-Null
  $envDir = Join-Path $Dir $EnvName
  New-Item -ItemType Directory -Path $envDir -Force | Out-Null

  $binBytes = [byte[]](1..64)
  $mergedBytes = [byte[]](65..200)
  [System.IO.File]::WriteAllBytes((Join-Path $envDir 'firmware.bin'), $binBytes)
  [System.IO.File]::WriteAllBytes((Join-Path $envDir 'firmware-merged.bin'), $mergedBytes)
  Write-TextFileLf (Join-Path $envDir 'firmware.elf') "fake-elf"
  Write-TextFileLf (Join-Path $envDir 'firmware.map') "fake-map"
  Write-TextFileLf (Join-Path $envDir 'build.log') "fake-build"
  Write-TextFileLf (Join-Path $envDir 'merge.log') "fake-merge"
  Write-TextFileLf (Join-Path $envDir 'nm-symbols.txt') "fake-nm"

  $defRel = "$EnvName/firmware-merged.bin"
  $fxPolicy = [ordered]@{
    schema = 'plane-radar/release-policy'; schema_version = 1
    local_only = [ordered]@{ release_root_relative = 'release' }
    default_artifact = [ordered]@{ environment = $EnvName; relative_path = $defRel; role = 'default-release' }
    environments = @([ordered]@{
        name = $EnvName; role = 'default-release'; evaluation_only = $false
        options = [ordered]@{ worker = $false; diagnostics = $false; log_level = 2 }
        resources = [ordered]@{ ram = 1; flash = 2; firmware_bin = $binBytes.Length; merged_bin = $mergedBytes.Length }
      })
  }
  $policyPath = Join-Path $Dir 'policy.json'
  Write-JsonFileLf $policyPath $fxPolicy

  $proof = [ordered]@{
    schema = 'plane-radar/binary-proof'; schemaVersion = 1; commit = $Commit
    overall = 'pass'; total = 1; passed = 1; failed = 0
    invariants = @([ordered]@{ id = 'fixture'; category = 'fixture'; environment = $EnvName; description = 'fixture'; expectation = 'x'; observed = 'x'; status = 'pass' })
  }
  Write-JsonFileLf (Join-Path $Dir 'binary-proof.json') $proof
  Write-TextFileLf (Join-Path $Dir 'binary-proof.txt') "fixture proof"

  $artifactsMap = New-PackageArtifactsMap -PackageDir $Dir
  $mergedSha = $artifactsMap[$defRel].sha256
  $manifest = [ordered]@{
    schema = 'plane-radar/release-manifest'; schemaVersion = 1
    git = [ordered]@{ commit = $Commit; short = $Commit.Substring(0, 7); branch = 'fixture' }
    default_artifact = [ordered]@{ environment = $EnvName; relative_path = $defRel; role = 'default-release'; size = $mergedBytes.Length; sha256 = $mergedSha }
    build = [ordered]@{ clean_build = $true; certified = $true }
    proof_summary = [ordered]@{ overall = 'pass'; total = 1; passed = 1; failed = 0 }
    environments = @([ordered]@{
        name = $EnvName; role = 'default-release'; evaluation_only = $false
        options = [ordered]@{ worker = $false; diagnostics = $false; log_level = 2 }
        resources = [ordered]@{ ram = 1; flash = 2; firmware_bin = $binBytes.Length; merged_bin = $mergedBytes.Length }
      })
    artifacts = $artifactsMap
  }
  Write-JsonFileLf (Join-Path $Dir 'manifest.json') $manifest
  Write-PackageChecksums -PackageDir $Dir

  return @{
    PolicyPath = $policyPath
    DefaultRel = $defRel
    ImageSize  = [int64]$mergedBytes.Length
    ImageSha256 = $mergedSha
    Environment = $EnvName
  }
}

# ---------------------------------------------------------------------------
# Hardware acceptance evidence (shared by initialize/verify-hardware-evidence)
# ---------------------------------------------------------------------------

function Get-ManifestImageMaps {
  # From a parsed release manifest, return @{ Sha=@{rel->sha}; Size=@{rel->size} }
  # for every artifact -- used to bind each hardware item to its flashed image.
  [OutputType([hashtable])]
  param([Parameter(Mandatory)]$Manifest)
  $sha = @{}; $size = @{}
  $artifacts = Get-PsObjectProperty $Manifest 'artifacts'
  foreach ($p in $artifacts.PSObject.Properties) {
    $sha[$p.Name] = [string]$p.Value.sha256
    $size[$p.Name] = [int64]$p.Value.size
  }
  return @{ Sha = $sha; Size = $size }
}

function New-HardwareResultsScaffold {
  # Build a PENDING hardware-results object from the acceptance policy, bound to
  # the exact flashed default image. Every item starts status='pending' with
  # numeric thresholds (measured=null), exact expected evidence filenames, and
  # operator fields. NEVER fabricates passing evidence.
  [OutputType([System.Collections.Specialized.OrderedDictionary])]
  param(
    [Parameter(Mandatory)]$Policy,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$DefaultRel,
    [Parameter(Mandatory)][string]$DefaultEnv,
    [Parameter(Mandatory)][hashtable]$ImageSha,
    [Parameter(Mandatory)][hashtable]$ImageSize,
    [Parameter(Mandatory)][string]$GeneratedUtc
  )
  $items = New-Object System.Collections.Generic.List[object]
  foreach ($pi in $Policy.items) {
    $imgRel = [string]$pi.image
    $imgSha = ''; if ($ImageSha.ContainsKey($imgRel)) { $imgSha = $ImageSha[$imgRel] }
    $imgSize = 0; if ($ImageSize.ContainsKey($imgRel)) { $imgSize = $ImageSize[$imgRel] }
    $thr = New-Object System.Collections.Generic.List[object]
    foreach ($t in $pi.thresholds) {
      $thr.Add([ordered]@{ key = $t.key; op = $t.op; value = $t.value; unit = $t.unit; measured = $null })
    }
    $ev = New-Object System.Collections.Generic.List[object]
    foreach ($e in $pi.evidence) {
      $ev.Add([ordered]@{ name = $e.name; description = $e.description; present = $false })
    }
    $informational = $false
    $infoProp = $pi.PSObject.Properties['informational']
    if ($infoProp -and [bool]$infoProp.Value) { $informational = $true }
    $items.Add([ordered]@{
      id            = $pi.id
      gate          = $pi.gate
      order         = $pi.order
      title         = $pi.title
      mandatory     = [bool]$pi.mandatory
      informational = $informational
      environment   = $pi.environment
      image         = $imgRel
      image_sha256  = $imgSha
      image_size    = $imgSize
      status        = 'pending'
      thresholds    = $thr.ToArray()
      evidence      = $ev.ToArray()
      operator      = [ordered]@{ name = ''; date = ''; notes = '' }
    })
  }
  return [ordered]@{
    schema         = 'plane-radar/hardware-results'
    schema_version = 1
    generated_utc  = $GeneratedUtc
    policy_ref     = 'scripts/hardware-acceptance-policy.json'
    binding        = [ordered]@{
      commit        = $Commit
      environment   = $DefaultEnv
      default_image = $DefaultRel
      image_size    = $ImageSize[$DefaultRel]
      image_sha256  = $ImageSha[$DefaultRel]
      flash_offset  = '0x0'
      note          = 'Evidence is bound to the EXACT flashed default merged image SHA-256. Worker items bind to the worker-diag image SHA.'
    }
    gates          = $Policy.gates
    items          = $items.ToArray()
  }
}

function Test-Threshold {
  # Numeric ops (>= <= > <) compare as doubles; equality ops (== !=) compare as
  # strings so 'yes'/'no'/'pass' and numeric-as-string both work. A null (never
  # measured) value always fails.
  [OutputType([bool])]
  param([string]$Op, $Measured, $Expected)
  if ($null -eq $Measured) { return $false }
  if ("$Measured".Trim().Length -eq 0 -and $Op -ne '==' -and $Op -ne '!=') { return $false }
  switch ($Op) {
    '>=' { $m = 0.0; $e = 0.0; if (-not [double]::TryParse("$Measured", [ref]$m)) { return $false }; if (-not [double]::TryParse("$Expected", [ref]$e)) { return $false }; return ($m -ge $e) }
    '<=' { $m = 0.0; $e = 0.0; if (-not [double]::TryParse("$Measured", [ref]$m)) { return $false }; if (-not [double]::TryParse("$Expected", [ref]$e)) { return $false }; return ($m -le $e) }
    '>'  { $m = 0.0; $e = 0.0; if (-not [double]::TryParse("$Measured", [ref]$m)) { return $false }; if (-not [double]::TryParse("$Expected", [ref]$e)) { return $false }; return ($m -gt $e) }
    '<'  { $m = 0.0; $e = 0.0; if (-not [double]::TryParse("$Measured", [ref]$m)) { return $false }; if (-not [double]::TryParse("$Expected", [ref]$e)) { return $false }; return ($m -lt $e) }
    '==' { return ("$Measured" -eq "$Expected") }
    '!=' { return ("$Measured" -ne "$Expected") }
    default { return $false }
  }
}

function Test-HardwareEvidence {
  # Validate a hardware-results.json + its evidence for one gate. Returns
  # { Ok; Errors; Checks }. Thresholds/required evidence are read from the
  # AUTHORITATIVE policy (not the operator-editable results copy); measured
  # values + status come from results. Never auto-promotes anything.
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$EvidenceDir,
    [Parameter(Mandatory)][string]$ResultsPath,
    [Parameter(Mandatory)]$Policy,
    [Parameter(Mandatory)][hashtable]$ImageSha,
    [Parameter(Mandatory)][hashtable]$ImageSize,
    [Parameter(Mandatory)][string]$ExpectedCommit,
    [Parameter(Mandatory)][string]$ExpectedEnv,
    [Parameter(Mandatory)][string]$ExpectedDefaultRel,
    [string]$Gate = 'default-release',
    [switch]$Quiet
  )
  $errors = New-Object System.Collections.Generic.List[string]
  $checks = New-Object System.Collections.Generic.List[object]
  function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $status = 'FAIL'; if ($ok) { $status = 'pass' }
    $checks.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    if (-not $ok) { $errors.Add("$name -- $detail") }
    if (-not $Quiet) {
      $color = 'Green'; if (-not $ok) { $color = 'Red' }
      $suffix = ''; if ($detail) { $suffix = " -- $detail" }
      Write-Host ("  [{0}] {1}{2}" -f $status, $name, $suffix) -ForegroundColor $color
    }
  }

  if (-not (Test-Path $ResultsPath)) {
    Add-Check 'results-exists' $false "missing $ResultsPath"
    return [pscustomobject]@{ Ok = $false; Errors = $errors; Checks = $checks }
  }
  $results = Read-JsonFile $ResultsPath
  $evDirFull = Get-FullPathSafe $EvidenceDir

  # -- binding to the exact flashed default image ----------------------------
  $b = Get-PsObjectProperty $results 'binding'
  Add-Check 'binding-commit' (([string](Get-PsObjectProperty $b 'commit')) -eq $ExpectedCommit) "results must bind to release commit $ExpectedCommit"
  Add-Check 'binding-environment' (([string](Get-PsObjectProperty $b 'environment')) -eq $ExpectedEnv) "default environment must be $ExpectedEnv"
  Add-Check 'binding-default-image' (([string](Get-PsObjectProperty $b 'default_image')) -eq $ExpectedDefaultRel) "default image path must be $ExpectedDefaultRel"
  $expDefSha = ''; if ($ImageSha.ContainsKey($ExpectedDefaultRel)) { $expDefSha = $ImageSha[$ExpectedDefaultRel] }
  $expDefSize = -1; if ($ImageSize.ContainsKey($ExpectedDefaultRel)) { $expDefSize = $ImageSize[$ExpectedDefaultRel] }
  Add-Check 'binding-image-sha' (([string](Get-PsObjectProperty $b 'image_sha256')) -eq $expDefSha) "binding image_sha256 must equal the flashed default image SHA-256"
  Add-Check 'binding-image-size' ([int64](Get-PsObjectProperty $b 'image_size') -eq [int64]$expDefSize) "binding image_size must equal the flashed default image byte length"

  # -- index results items ----------------------------------------------------
  $resById = @{}
  foreach ($ri in @(Get-PsObjectProperty $results 'items')) { $resById[[string](Get-PsObjectProperty $ri 'id')] = $ri }

  $selected = @($Policy.items | Where-Object { $Gate -eq 'all' -or $_.gate -eq $Gate })
  if ($selected.Count -eq 0) { Add-Check 'gate-has-items' $false "no policy items for gate '$Gate'" }

  foreach ($pi in $selected) {
    $id = [string]$pi.id
    if (-not $resById.ContainsKey($id)) { Add-Check "item/$id" $false "item missing from results"; continue }
    $ri = $resById[$id]
    $informational = $false
    $infoProp = $pi.PSObject.Properties['informational']
    if ($infoProp -and [bool]$infoProp.Value) { $informational = $true }

    # per-item binding to ITS flashed image
    $imgRel = [string]$pi.image
    $expSha = ''; if ($ImageSha.ContainsKey($imgRel)) { $expSha = $ImageSha[$imgRel] }
    Add-Check "item-image-sha/$id" (([string](Get-PsObjectProperty $ri 'image_sha256')) -eq $expSha -and $expSha.Length -eq 64) "item must bind to the flashed $imgRel SHA-256"

    # every results-declared evidence filename must be SAFE (blocks traversal/absolute)
    foreach ($rev in @(Get-PsObjectProperty $ri 'evidence')) {
      $rn = [string](Get-PsObjectProperty $rev 'name')
      Add-Check "evidence-path/$id/$rn" (Test-RelPathSafe $rn) "evidence filename must be a safe relative path"
    }

    # every POLICY-required evidence file must exist + be non-empty
    foreach ($pe in $pi.evidence) {
      $name = [string]$pe.name
      $safe = Test-RelPathSafe $name
      $file = Join-Path $evDirFull ($name -replace '/', '\')
      $inside = $safe -and (Test-PathInside -Base $evDirFull -Candidate $file)
      $exists = $inside -and (Test-Path $file -PathType Leaf)
      $nonEmpty = $exists -and ((Get-Item $file).Length -gt 0)
      Add-Check "evidence/$id/$name" ($safe -and $inside -and $exists -and $nonEmpty) "required evidence must exist and be non-empty"
    }

    if ($informational) { continue }  # recorded but not gating

    # status must be an explicit pass
    $status = [string](Get-PsObjectProperty $ri 'status')
    Add-Check "status/$id" ($status -eq 'pass') "status must be 'pass' (got '$status'; pending/blocked/waived/fail are NOT pass)"

    # measured values must satisfy the policy thresholds
    $measuredByKey = @{}
    foreach ($rt in @(Get-PsObjectProperty $ri 'thresholds')) { $measuredByKey[[string](Get-PsObjectProperty $rt 'key')] = (Get-PsObjectProperty $rt 'measured') }
    foreach ($pt in $pi.thresholds) {
      $key = [string]$pt.key
      $measured = $null; if ($measuredByKey.ContainsKey($key)) { $measured = $measuredByKey[$key] }
      $ok = Test-Threshold -Op ([string]$pt.op) -Measured $measured -Expected $pt.value
      Add-Check "threshold/$id/$key" $ok "measured '$measured' must be $($pt.op) $($pt.value) $($pt.unit)"
    }
  }

  return [pscustomobject]@{ Ok = ($errors.Count -eq 0); Errors = $errors; Checks = $checks }
}
