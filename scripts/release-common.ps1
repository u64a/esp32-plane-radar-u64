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

function Find-ReparsePointInChain {
  # Return the OUTERMOST existing reparse point (junction/symlink) on the path
  # chain from $Base (inclusive) down to $Full (inclusive), or $null if none.
  # Lexical containment (Test-PathInside) is NOT enough on Windows: any level of
  # the chain could be a junction/symlink that redirects the real target. Both
  # paths are resolved lexically first; $Full is expected to be inside/equal $Base.
  # FAIL-CLOSED: Test-IsReparsePoint throws if an existing level cannot be inspected.
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$Full)
  $baseFull = (Get-FullPathSafe $Base).TrimEnd('\', '/')
  $full = (Get-FullPathSafe $Full).TrimEnd('\', '/')
  $levels = New-Object System.Collections.Generic.List[string]
  $cur = $full
  while ($true) {
    $levels.Add($cur)
    if ($cur.Equals($baseFull, [System.StringComparison]::OrdinalIgnoreCase)) { break }
    $parent = Split-Path $cur -Parent
    if ([string]::IsNullOrEmpty($parent)) { break }
    $parent = $parent.TrimEnd('\', '/')
    if ($parent.Equals($cur, [System.StringComparison]::OrdinalIgnoreCase)) { break }
    if (-not (Test-PathInside -Base $baseFull -Candidate $parent -AllowEqual)) { break }
    $cur = $parent
  }
  for ($i = $levels.Count - 1; $i -ge 0; $i--) {
    $p = $levels[$i]
    if (Test-Path -LiteralPath $p) {
      if (Test-IsReparsePoint $p) { return $p }
    }
  }
  return $null
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

function Test-IsoCalendarDate {
  # True iff $Value is a REAL calendar date in strict yyyy-MM-dd form (invariant
  # culture, no style adjustment). Rejects malformed strings (e.g. 01/02/2026) AND
  # impossible dates (e.g. 2026-99-99, 2026-02-30) that a regex-only check accepts.
  [OutputType([bool])]
  param([string]$Value)
  if ($null -eq $Value) { return $false }
  if (-not [regex]::IsMatch($Value, '^\d{4}-\d{2}-\d{2}$')) { return $false }
  $dt = [datetime]::MinValue
  return [datetime]::TryParseExact($Value, 'yyyy-MM-dd', [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::None, [ref]$dt)
}

# ---------------------------------------------------------------------------
# Reparse-point-safe recursive deletion (Windows junction/symlink defence)
# ---------------------------------------------------------------------------

function Test-IsReparsePoint {
  # True if $Path is a Windows reparse point (directory junction or symlink).
  # FAIL-CLOSED: a genuinely non-existent path is simply "not a reparse point"
  # (false), but an EXISTING path that cannot be inspected THROWS -- we must never
  # silently treat an un-inspectable path as safe before a recursive delete.
  [OutputType([bool])]
  param([Parameter(Mandatory)][string]$Path)
  $it = $null
  try { $it = Get-Item -LiteralPath $Path -Force -ErrorAction Stop }
  catch [System.Management.Automation.ItemNotFoundException] { return $false }
  catch { throw "Cannot inspect path for reparse-point check (failing closed): $Path -- $($_.Exception.Message)" }
  if ($null -eq $it) { throw "Cannot inspect path for reparse-point check (failing closed): $Path" }
  return (($it.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq [System.IO.FileAttributes]::ReparsePoint)
}

function Get-ContainedReparsePoints {
  # Enumerate every reparse point (junction/symlink) at or under $Root WITHOUT ever
  # descending THROUGH one (we only read attributes and never follow a reparse
  # target). Returns the full paths of any reparse points found.
  # FAIL-CLOSED: if any directory under $Root cannot be enumerated we THROW rather
  # than silently continuing -- an un-enumerable directory could hide a junction
  # that would redirect a subsequent recursive delete.
  [OutputType([string[]])]
  param([Parameter(Mandatory)][string]$Root)
  $hits = New-Object System.Collections.Generic.List[string]
  if (Test-IsReparsePoint $Root) { $hits.Add((Get-FullPathSafe $Root)) }
  $stack = New-Object System.Collections.Generic.Stack[string]
  if (Test-Path -LiteralPath $Root -PathType Container) { $stack.Push((Get-FullPathSafe $Root)) }
  while ($stack.Count -gt 0) {
    $dir = $stack.Pop()
    $children = $null
    try { $children = @(Get-ChildItem -LiteralPath $dir -Force -ErrorAction Stop) }
    catch { throw "Cannot enumerate directory for reparse-point check (failing closed): $dir -- $($_.Exception.Message)" }
    foreach ($c in $children) {
      $isReparse = (($c.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq [System.IO.FileAttributes]::ReparsePoint)
      if ($isReparse) { $hits.Add((Get-FullPathSafe $c.FullName)); continue }  # never descend into it
      if ($c.PSIsContainer) { $stack.Push($c.FullName) }
    }
  }
  if ($hits.Count -eq 0) { return @() }
  return , ($hits.ToArray())
}

function Remove-TreeSafe {
  # The ONLY sanctioned recursive delete in the release pipeline. It proves the
  # exact allowed base (and, optionally, an exact leaf name) and refuses to delete
  # THROUGH or ACROSS any Windows reparse point:
  #   * the resolved target must live strictly inside $AllowedBase (or equal it
  #     when -AllowEqualBase);
  #   * an optional -RequireLeaf must match the target's leaf exactly;
  #   * the target root must NOT itself be a reparse point (junction/symlink);
  #   * NO reparse point may exist anywhere under the target before recursion.
  # This stops a planted junction from redirecting a -Recurse delete outside the
  # intended tree.
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][string]$AllowedBase,
    [switch]$AllowEqualBase,
    [string]$RequireLeaf
  )
  $full = Get-FullPathSafe $Path
  if (-not (Test-PathInside -Base $AllowedBase -Candidate $full -AllowEqual:$AllowEqualBase)) {
    throw "Refusing to delete '$full': not inside allowed base '$AllowedBase'."
  }
  if ($RequireLeaf) {
    $leaf = Split-Path $full -Leaf
    if ($leaf -ne $RequireLeaf) { throw "Refusing to delete '$full': leaf '$leaf' != required '$RequireLeaf'." }
  }
  if (-not (Test-Path -LiteralPath $full)) { return }
  if (Test-IsReparsePoint $full) {
    throw "Refusing to recursively delete a reparse point (junction/symlink) at the target: $full"
  }
  $contained = @(Get-ContainedReparsePoints $full | Where-Object { $_ -and ("$_").Length -gt 0 })
  if ($contained.Count -gt 0) {
    throw ("Refusing to recurse: reparse point(s) present under '$full': " + [string]::Join('; ', @($contained | Select-Object -First 5)))
  }
  Remove-Item -LiteralPath $full -Recurse -Force
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
  if ($branchRes.ExitCode -ne 0) { throw "git rev-parse --abbrev-ref HEAD failed (exit $($branchRes.ExitCode)) in $RepoRoot." }
  $branch = ([string]($branchRes.Output | Select-Object -First 1)).Trim()
  if ($branch.Length -eq 0) { throw "git returned an empty branch name in $RepoRoot." }

  $remoteRes = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('remote')
  if ($remoteRes.ExitCode -ne 0) { throw "git remote failed (exit $($remoteRes.ExitCode)) in $RepoRoot." }
  $remotes = @($remoteRes.Output | Where-Object { $_ -and $_.Trim().Length -gt 0 })

  $statusRes = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('status', '--porcelain=v1', '--untracked-files=all')
  if ($statusRes.ExitCode -ne 0) { throw "git status --porcelain failed (exit $($statusRes.ExitCode)) in $RepoRoot." }
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
# Tracked-git policy authority (the ONLY certification source of truth)
#
# Policy is loaded from an EXACT git commit blob (`git show <commit>:<path>`),
# NEVER the editable working tree, so a dirty/attacker-modified working-tree
# policy cannot influence certification. The SHA-256 recorded in the manifest /
# hardware results is computed over these exact git-blob bytes and re-checked at
# verify time.
# ---------------------------------------------------------------------------

function Test-GitCommitPresent {
  # True iff $Commit resolves to a commit object in the local repo.
  [OutputType([bool])]
  param([Parameter(Mandatory)][string]$RepoRoot, [Parameter(Mandatory)][string]$Commit)
  if (-not (Test-Sha1Hex $Commit)) { return $false }
  $r = Invoke-GitRaw -RepoRoot $RepoRoot -GitArgs @('cat-file', '-e', ($Commit + '^{commit}'))
  return ($r.ExitCode -eq 0)
}

function Get-GitShowBytes {
  # Byte-exact content of a tracked file at an EXACT commit. Captures raw stdout
  # bytes (no newline/encoding mangling) so the SHA-256 is stable across shells and
  # unaffected by core.autocrlf working-tree smudging. Throws (fail-closed) on any
  # non-zero git exit (missing commit/path).
  [OutputType([byte[]])]
  param(
    [Parameter(Mandatory)][string]$RepoRoot,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$RelPath
  )
  if (-not (Test-Sha1Hex $Commit)) { throw "Get-GitShowBytes: '$Commit' is not a 40-hex commit." }
  # RelPath is an internal, fixed policy path (never operator-derived); still reject
  # anything outside a conservative safe charset to be defensive.
  if ($RelPath -notmatch '^[A-Za-z0-9_./-]+$') { throw "Get-GitShowBytes: unsafe rel path '$RelPath'." }
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = 'git'
  $psi.Arguments = ('-C "{0}" --no-pager show {1}:{2}' -f $RepoRoot, $Commit, $RelPath)
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  $ms = New-Object System.IO.MemoryStream
  try {
    $proc.StandardOutput.BaseStream.CopyTo($ms)
    $errText = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
  } finally { }
  if ($proc.ExitCode -ne 0) { throw "git show ${Commit}:${RelPath} failed (exit $($proc.ExitCode)) in ${RepoRoot}: $errText" }
  return $ms.ToArray()
}

function Get-GitPolicy {
  # Load a tracked policy JSON from an EXACT git commit. Returns
  # @{ Object=<parsed>; Sha256=<lowercase hex over the exact git-blob bytes> }.
  # The hash binds the EXACT bytes used for certification.
  [OutputType([hashtable])]
  param(
    [Parameter(Mandatory)][string]$RepoRoot,
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$RelPath
  )
  if (-not (Test-GitCommitPresent -RepoRoot $RepoRoot -Commit $Commit)) {
    throw "Commit $Commit is not present in the local repository; cannot load tracked policy '$RelPath' (fail-closed)."
  }
  $bytes = Get-GitShowBytes -RepoRoot $RepoRoot -Commit $Commit -RelPath $RelPath
  $sha = Get-Sha256HexOfBytes $bytes
  $text = [System.Text.Encoding]::UTF8.GetString($bytes)
  if ($text.Length -gt 0 -and [int]$text[0] -eq 0xFEFF) { $text = $text.Substring(1) }
  $obj = $null
  try { $obj = $text | ConvertFrom-Json } catch { throw "Tracked policy '$RelPath'@$Commit is not valid JSON: $($_.Exception.Message)" }
  return @{ Object = $obj; Sha256 = $sha }
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
# Observed (installed) toolchain/library versions from package metadata
# ---------------------------------------------------------------------------

function Get-PackageJsonVersion {
  # Return the installed version of a PlatformIO package by reading its
  # package.json / .piopkgmanifest.json. Fail closed if the package is missing or
  # ambiguously installed under more than one version directory.
  [OutputType([string])]
  param(
    [Parameter(Mandatory)][string]$PackagesDir,
    [Parameter(Mandatory)][string]$PackageName
  )
  $candidates = New-Object System.Collections.Generic.List[object]
  foreach ($d in (Get-ChildItem -LiteralPath $PackagesDir -Directory -ErrorAction SilentlyContinue)) {
    $pj = Join-Path $d.FullName 'package.json'
    if (-not (Test-Path $pj)) { continue }
    $obj = $null
    try { $obj = Get-Content -Raw -LiteralPath $pj | ConvertFrom-Json } catch { continue }
    $nm = [string](Get-PsObjectProperty $obj 'name')
    if ($nm -eq $PackageName) {
      $candidates.Add([pscustomobject]@{ Dir = $d.Name; Version = [string](Get-PsObjectProperty $obj 'version') })
    }
  }
  if ($candidates.Count -eq 0) { throw "PlatformIO package '$PackageName' not installed under $PackagesDir." }
  if ($candidates.Count -gt 1) {
    throw ("PlatformIO package '$PackageName' is installed under multiple version directories (" +
      [string]::Join(', ', @($candidates | ForEach-Object { $_.Dir })) + "); cannot determine a single observed version.")
  }
  $v = $candidates[0].Version
  if ([string]::IsNullOrWhiteSpace($v)) { throw "PlatformIO package '$PackageName' has no version in its package.json." }
  return $v
}

function Get-PlatformInstalledVersion {
  # Observed version of an installed PlatformIO platform (e.g. espressif32).
  [OutputType([string])]
  param(
    [Parameter(Mandatory)][string]$CoreDir,
    [Parameter(Mandatory)][string]$PlatformName
  )
  $platformsDir = Join-Path $CoreDir 'platforms'
  $matches = New-Object System.Collections.Generic.List[object]
  foreach ($d in (Get-ChildItem -LiteralPath $platformsDir -Directory -ErrorAction SilentlyContinue)) {
    $pj = Join-Path $d.FullName 'platform.json'
    if (-not (Test-Path $pj)) { continue }
    $obj = $null
    try { $obj = Get-Content -Raw -LiteralPath $pj | ConvertFrom-Json } catch { continue }
    if (([string](Get-PsObjectProperty $obj 'name')) -eq $PlatformName) {
      $matches.Add([string](Get-PsObjectProperty $obj 'version'))
    }
  }
  if ($matches.Count -eq 0) { throw "PlatformIO platform '$PlatformName' not installed under $platformsDir." }
  if ($matches.Count -gt 1) { throw "PlatformIO platform '$PlatformName' installed under multiple versions; ambiguous observed version." }
  return $matches[0]
}

function Get-LibInstalledVersion {
  # Observed resolved version of a project library dependency, from the per-env
  # libdeps directory (library.json / .piopkgmanifest.json).
  [OutputType([string])]
  param(
    [Parameter(Mandatory)][string]$LibDepsDir,
    [Parameter(Mandatory)][string]$LibDirName
  )
  $dir = Join-Path $LibDepsDir $LibDirName
  if (-not (Test-Path $dir)) { throw "Library '$LibDirName' not resolved under $LibDepsDir (build the env first)." }
  foreach ($meta in @('.piopkgmanifest.json', 'library.json')) {
    $mp = Join-Path $dir $meta
    if (Test-Path $mp) {
      $obj = Get-Content -Raw -LiteralPath $mp | ConvertFrom-Json
      $v = [string](Get-PsObjectProperty $obj 'version')
      if (-not [string]::IsNullOrWhiteSpace($v)) { return $v }
    }
  }
  throw "Could not read an installed version for library '$LibDirName' under $LibDepsDir."
}

function Get-ObservedToolVersions {
  # Gather the OBSERVED installed toolchain + library versions that the build
  # actually used, from package metadata. $Policy supplies the dependency dir
  # names; $LibDepsDir is .pio/libdeps/<default-env>.
  [OutputType([System.Collections.Specialized.OrderedDictionary])]
  param(
    [Parameter(Mandatory)]$Policy,
    [Parameter(Mandatory)][string]$LibDepsDir
  )
  $core = Resolve-PlatformIoCoreDir
  $packagesDir = Join-Path $core 'packages'
  $deps = [ordered]@{}
  foreach ($p in $Policy.pins.dependencies.PSObject.Properties) {
    $libDirName = ($p.Name -split '/')[-1]   # e.g. lovyan03/LovyanGFX -> LovyanGFX
    $deps[$p.Name] = (Get-LibInstalledVersion -LibDepsDir $LibDepsDir -LibDirName $libDirName)
  }
  return [ordered]@{
    platform_version             = (Get-PlatformInstalledVersion -CoreDir $core -PlatformName 'espressif32')
    framework_arduinoespressif32 = (Get-PackageJsonVersion -PackagesDir $packagesDir -PackageName 'framework-arduinoespressif32')
    toolchain_riscv32_esp        = (Get-PackageJsonVersion -PackagesDir $packagesDir -PackageName 'toolchain-riscv32-esp')
    tool_esptoolpy               = (Get-PackageJsonVersion -PackagesDir $packagesDir -PackageName 'tool-esptoolpy')
    dependencies                 = $deps
  }
}

function Get-PolicyPlatformVersion {
  # The bare version portion of a policy platform pin string ("espressif32@6.5.0" -> "6.5.0").
  [OutputType([string])]
  param([Parameter(Mandatory)][string]$PlatformPin)
  $at = $PlatformPin.LastIndexOf('@')
  if ($at -ge 0) { return $PlatformPin.Substring($at + 1) }
  return $PlatformPin
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
# Binary proof engine (SHARED by build-release and verify-release)
#
# The SAME derivation is used to (a) generate binary-proof.json at build time and
# (b) INDEPENDENTLY re-derive the expected invariants from the packaged ELF/bin at
# verify time. Both paths bind the EXACT current-head binaries only.
# ---------------------------------------------------------------------------

function Get-ProofSpec {
  # Central, single-source definition of every proof token/needle. Returned so
  # build + verify derive byte-for-byte identical invariants.
  [OutputType([hashtable])]
  param()
  $workerTokens = @('workerTask', 's_worker_stack', 's_worker_tcb', 's_request_q', 's_result_q', 'workerCancel')
  $diagSymTokens = @('g_diag_last_fetch_ms', 'radarDisplayLastDiagnostics')
  $heapApiTokens = @('getFreeHeap', 'getMinFreeHeap', 'getMaxAllocHeap', 'heap_caps_get_minimum_free_size')
  $heapSharedTokens = @('heap_caps_get_free_size', 'heap_caps_get_largest_free_block')
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
  return @{
    WorkerTokens     = $workerTokens
    DiagSymTokens    = $diagSymTokens
    HeapApiTokens    = $heapApiTokens
    HeapSharedTokens = $heapSharedTokens
    EvidenceTokens   = @($workerTokens + $diagSymTokens + $heapApiTokens + $heapSharedTokens)
    DiagStrings      = $diagStringNeedles
    LogStrings       = $logStringNeedles
    PlaneSubstr      = (Get-AsciiBytesWithTrailer -Text 'Plane Radar')
  }
}

function Get-EnvProofFlags {
  # Worker/Diag/Quiet booleans for an environment from the policy.
  [OutputType([pscustomobject])]
  param([Parameter(Mandatory)]$Policy, [Parameter(Mandatory)][string]$Name)
  $ep = $Policy.environments | Where-Object { $_.name -eq $Name } | Select-Object -First 1
  if ($null -eq $ep) { throw "environment '$Name' not present in policy." }
  return [pscustomobject]@{
    Worker = [bool]$ep.options.worker
    Diag   = [bool]$ep.options.diagnostics
    Quiet  = ([int]$ep.options.log_level -eq 0)
  }
}

function ConvertTo-ProofYesNo { param([bool]$b) if ($b) { return 'present' } else { return 'absent' } }

function Get-MergedLayoutInvariants {
  # Re-derive the firmware-merged.bin layout invariants for ONE environment
  # DIRECTLY from the packaged bytes (binary-safe; NEVER trusts merge.log text):
  #   * exact policy SHA-256 anchor for firmware.bin AND firmware-merged.bin
  #     (the shipped image is bound to TRACKED policy, not self-consistent hashes);
  #   * merged length == app_offset + firmware.bin length;
  #   * each policy component region SHA-256 matches at its fixed offset;
  #   * every gap between a component end and the next offset is gap_fill_byte;
  #   * the app region (merged[app_offset..]) is byte-for-byte firmware.bin.
  [OutputType([object[]])]
  param(
    [Parameter(Mandatory)]$Policy,
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$MergedPath,
    [Parameter(Mandatory)][string]$FirmwarePath
  )
  $ml = Get-PsObjectProperty $Policy 'merged_layout'
  if ($null -eq $ml) { throw "policy has no merged_layout section." }
  $appOffset = [int64](Get-PsObjectProperty $ml 'app_offset')
  $fill = [byte]([int](Get-PsObjectProperty $ml 'gap_fill_byte'))
  $envPolicy = $Policy.environments | Where-Object { $_.name -eq $Name } | Select-Object -First 1
  if ($null -eq $envPolicy) { throw "environment '$Name' not in policy." }
  $envSha = Get-PsObjectProperty $envPolicy 'sha256'

  $merged = [System.IO.File]::ReadAllBytes($MergedPath)
  $app = [System.IO.File]::ReadAllBytes($FirmwarePath)
  $inv = New-Object System.Collections.Generic.List[object]

  # -- Exact policy SHA-256 anchors (external anchor) -------------------------
  $fwSha = Get-Sha256HexOfBytes $app
  $mgSha = Get-Sha256HexOfBytes $merged
  $expFw = ''; if ($envSha) { $expFw = [string](Get-PsObjectProperty $envSha 'firmware_bin') }
  $expMg = ''; if ($envSha) { $expMg = [string](Get-PsObjectProperty $envSha 'merged_bin') }
  $inv.Add((New-InvariantResult -Id "image-sha/$Name/firmware.bin" -Category 'image-sha' -Environment $Name `
    -Description 'firmware.bin exact policy SHA-256 anchor' -Expectation $expFw -Observed $fwSha -Pass (($expFw.Length -eq 64) -and ($fwSha -eq $expFw))))
  $inv.Add((New-InvariantResult -Id "image-sha/$Name/firmware-merged.bin" -Category 'image-sha' -Environment $Name `
    -Description 'firmware-merged.bin exact policy SHA-256 anchor (the SHIPPED image)' -Expectation $expMg -Observed $mgSha -Pass (($expMg.Length -eq 64) -and ($mgSha -eq $expMg))))

  # -- merged length == app_offset + firmware.bin length ----------------------
  $expLen = $appOffset + [int64]$app.Length
  $inv.Add((New-InvariantResult -Id "merged-length/$Name" -Category 'merged-layout' -Environment $Name `
    -Description 'merged length == app_offset + firmware.bin length' -Expectation "$expLen" -Observed "$($merged.Length)" -Pass ([int64]$merged.Length -eq $expLen)))

  # -- Component regions + gaps (sorted by offset) ----------------------------
  $sorted = @(@($ml.components) | Sort-Object { [int64]$_.offset })
  for ($i = 0; $i -lt $sorted.Count; $i++) {
    $c = $sorted[$i]
    $off = [int64]$c.offset; $sz = [int64]$c.size; $expSha = [string]$c.sha256
    $regionOk = $false; $obs = 'out-of-range'
    if (($off + $sz) -le [int64]$merged.Length -and $off -ge 0) {
      $seg = New-Object byte[] $sz
      [Array]::Copy($merged, $off, $seg, 0, $sz)
      $obs = Get-Sha256HexOfBytes $seg
      $regionOk = (($expSha.Length -eq 64) -and ($obs -eq $expSha))
    }
    $inv.Add((New-InvariantResult -Id "merged-comp/$Name/$($c.name)" -Category 'merged-layout' -Environment $Name `
      -Description "merged component '$($c.name)' region SHA-256 at offset $off (size $sz)" -Expectation $expSha -Observed $obs -Pass $regionOk))
    $nextOff = $appOffset
    if ($i -lt ($sorted.Count - 1)) { $nextOff = [int64]$sorted[$i + 1].offset }
    $gapStart = $off + $sz
    $gapBad = -1
    if ($gapStart -ge 0 -and $nextOff -ge $gapStart -and $nextOff -le [int64]$merged.Length) {
      $gapBad = 0
      for ($j = $gapStart; $j -lt $nextOff; $j++) { if ($merged[$j] -ne $fill) { $gapBad++ } }
    }
    $gapObs = 'out-of-range'; if ($gapBad -ge 0) { $gapObs = "$gapBad non-fill byte(s)" }
    $inv.Add((New-InvariantResult -Id "merged-gap/$Name/$($c.name)" -Category 'merged-layout' -Environment $Name `
      -Description ("gap [{0}..{1}) after '{2}' is all 0x{3}" -f $gapStart, $nextOff, $c.name, $fill.ToString('X2')) -Expectation 'all-fill' -Observed $gapObs -Pass ($gapBad -eq 0)))
  }

  # -- App region byte-for-byte equal to firmware.bin -------------------------
  $appOk = $false; $appObs = 'length-mismatch'
  if (($appOffset + [int64]$app.Length) -eq [int64]$merged.Length -and $appOffset -ge 0) {
    $appOk = $true
    for ($k = 0; $k -lt $app.Length; $k++) { if ($merged[$appOffset + $k] -ne $app[$k]) { $appOk = $false; break } }
    if ($appOk) { $appObs = 'equal' } else { $appObs = 'byte-mismatch' }
  }
  $inv.Add((New-InvariantResult -Id "merged-app/$Name" -Category 'merged-layout' -Environment $Name `
    -Description 'merged app region (at app_offset) is byte-for-byte firmware.bin' -Expectation 'equal' -Observed $appObs -Pass $appOk))

  return [object[]]$inv.ToArray()
}

function Get-BinaryProofInvariants {
  # Derive the full per-environment invariant set (and the compact nm evidence)
  # from the ELF + firmware.bin of every policy environment, using the pinned nm.
  #   $EnvElf : ordered/hashtable env-name -> firmware.elf path
  #   $EnvBin : ordered/hashtable env-name -> firmware.bin path
  # Returns @{ Invariants = [object[]]; NmEvidence = @{env->string[]} }.
  [OutputType([hashtable])]
  param(
    [Parameter(Mandatory)]$Policy,
    [Parameter(Mandatory)][hashtable]$EnvElf,
    [Parameter(Mandatory)][hashtable]$EnvBin,
    [Parameter(Mandatory)][hashtable]$EnvMerged,
    [Parameter(Mandatory)][string]$NmPath
  )
  $spec = Get-ProofSpec
  $invariants = New-Object System.Collections.Generic.List[object]
  $nmEvidence = @{}
  foreach ($envPolicy in $Policy.environments) {
    $name = $envPolicy.name
    if (-not $EnvElf.ContainsKey($name)) { throw "missing ELF path for env '$name'." }
    if (-not $EnvBin.ContainsKey($name)) { throw "missing firmware.bin path for env '$name'." }
    if (-not $EnvMerged.ContainsKey($name)) { throw "missing firmware-merged.bin path for env '$name'." }
    $flags = Get-EnvProofFlags $Policy $name
    $nmLines = Get-NmLines -NmPath $NmPath -ElfPath $EnvElf[$name]
    $nmEvidence[$name] = Select-NmEvidenceLines -NmLines $nmLines -Tokens $spec.EvidenceTokens
    $hay = Get-Latin1Haystack $EnvBin[$name]

    foreach ($tok in $spec.WorkerTokens) {
      $present = Test-NmSymbolPresent $nmLines $tok
      $invariants.Add((New-InvariantResult -Id "worker-sym/$name/$tok" -Category 'worker-symbol' -Environment $name `
        -Description "Worker/integration symbol '$tok' linkage" `
        -Expectation (ConvertTo-ProofYesNo $flags.Worker) -Observed (ConvertTo-ProofYesNo $present) -Pass ($present -eq $flags.Worker)))
    }
    $stackSizes = Get-NmSymbolSizes $nmLines 's_worker_stack'
    $expCount = 0; if ($flags.Worker) { $expCount = 1 }
    $stackOk = ($stackSizes.Count -eq $expCount)
    if ($flags.Worker) { $stackOk = ($stackSizes.Count -eq 1 -and $stackSizes[0] -eq 8192) }
    $invariants.Add((New-InvariantResult -Id "worker-stack/$name" -Category 'worker-stack' -Environment $name `
      -Description "s_worker_stack symbol count/size (0x2000 = 8192 bytes)" `
      -Expectation ("count=$expCount" + $(if ($flags.Worker) { ', size=8192' } else { '' })) `
      -Observed ("count=$($stackSizes.Count), sizes=[$($stackSizes -join ',')]") -Pass $stackOk))

    foreach ($tok in $spec.DiagSymTokens) {
      $present = Test-NmSymbolPresent $nmLines $tok
      $invariants.Add((New-InvariantResult -Id "diag-sym/$name/$tok" -Category 'diag-symbol' -Environment $name `
        -Description "Diagnostic symbol '$tok'" `
        -Expectation (ConvertTo-ProofYesNo $flags.Diag) -Observed (ConvertTo-ProofYesNo $present) -Pass ($present -eq $flags.Diag)))
    }
    foreach ($tok in $spec.HeapApiTokens) {
      $present = Test-NmSymbolPresent $nmLines $tok
      $invariants.Add((New-InvariantResult -Id "heap-api/$name/$tok" -Category 'heap-api' -Environment $name `
        -Description "Heap diagnostic API indicator '$tok'" `
        -Expectation (ConvertTo-ProofYesNo $flags.Diag) -Observed (ConvertTo-ProofYesNo $present) -Pass ($present -eq $flags.Diag)))
    }
    foreach ($tok in $spec.HeapSharedTokens) {
      $present = Test-NmSymbolPresent $nmLines $tok
      $invariants.Add((New-InvariantResult -Id "heap-shared/$name/$tok" -Category 'heap-shared' -Environment $name `
        -Description "Shared framework heap symbol '$tok' (present in every build; NOT a diag indicator)" `
        -Expectation 'present' -Observed (ConvertTo-ProofYesNo $present) -Pass ($present)))
    }

    foreach ($label in $spec.DiagStrings.Keys) {
      $present = Test-HaystackContainsBytes $hay $spec.DiagStrings[$label]
      $invariants.Add((New-InvariantResult -Id "diag-str/$name/$label" -Category 'diag-string' -Environment $name `
        -Description "Diagnostic binary string '$label'" `
        -Expectation (ConvertTo-ProofYesNo $flags.Diag) -Observed (ConvertTo-ProofYesNo $present) -Pass ($present -eq $flags.Diag)))
    }
    foreach ($label in $spec.LogStrings.Keys) {
      $present = Test-HaystackContainsBytes $hay $spec.LogStrings[$label]
      $expected = -not $flags.Quiet
      $invariants.Add((New-InvariantResult -Id "quiet-log/$name/$label" -Category 'quiet-log' -Environment $name `
        -Description "Serial logging string '$label'" `
        -Expectation (ConvertTo-ProofYesNo $expected) -Observed (ConvertTo-ProofYesNo $present) -Pass ($present -eq $expected)))
    }
    $planePresent = Test-HaystackContainsBytes $hay $spec.PlaneSubstr
    $invariants.Add((New-InvariantResult -Id "quiet-log-baseline/$name" -Category 'quiet-log-baseline' -Environment $name `
      -Description "Plain 'Plane Radar' substring (portal HTML; present in every build, incl. quiet -- proves why plain substring is NOT a logging indicator)" `
      -Expectation 'present' -Observed (ConvertTo-ProofYesNo $planePresent) -Pass ($planePresent)))

    # Merged-layout + exact-SHA anchors, re-derived from the packaged firmware.bin
    # and the SHIPPED firmware-merged.bin (binary-safe; never trusts merge.log).
    foreach ($mlInv in @(Get-MergedLayoutInvariants -Policy $Policy -Name $name -MergedPath $EnvMerged[$name] -FirmwarePath $EnvBin[$name])) {
      $invariants.Add($mlInv)
    }
  }
  return @{ Invariants = $invariants.ToArray(); NmEvidence = $nmEvidence }
}

function Get-ProofCategoryCounts {
  # Ordered category -> count map over an invariant list (insertion order).
  [OutputType([System.Collections.Specialized.OrderedDictionary])]
  param([Parameter(Mandatory)][object[]]$Invariants)
  $cats = [ordered]@{}
  foreach ($inv in $Invariants) {
    $c = [string](Get-InvariantField $inv 'category')
    if (-not $cats.Contains($c)) { $cats[$c] = 0 }
    $cats[$c] = [int]$cats[$c] + 1
  }
  return $cats
}

function Get-InvariantField {
  # Uniform field access for invariant records that may be an OrderedDictionary
  # (freshly derived) OR a PSCustomObject (parsed from binary-proof.json).
  param([Parameter(Mandatory)]$Inv, [Parameter(Mandatory)][string]$Name)
  if ($Inv -is [System.Collections.IDictionary]) {
    if ($Inv.Contains($Name)) { return $Inv[$Name] }
    return $null
  }
  return (Get-PsObjectProperty $Inv $Name)
}

function Get-ProofOverall {
  # 'pass' iff every invariant status is 'pass' AND there is at least one.
  [OutputType([string])]
  param([Parameter(Mandatory)][object[]]$Invariants)
  if ($Invariants.Count -eq 0) { return 'fail' }
  foreach ($inv in $Invariants) { if (([string](Get-InvariantField $inv 'status')) -ne 'pass') { return 'fail' } }
  return 'pass'
}

function Get-PackageDerivedInvariants {
  # Production proof re-derivation used by the verifier: build the ELF/bin maps
  # from the PACKAGED per-env firmware.elf/firmware.bin and re-derive the exact
  # invariant set with the pinned nm. Fails closed if any binary is missing.
  [OutputType([object[]])]
  param([Parameter(Mandatory)][string]$PackageDir, [Parameter(Mandatory)]$Policy)
  $nmTool = Resolve-RiscvTool $Policy.toolchain_tools.nm
  $envElf = @{}; $envBin = @{}; $envMerged = @{}
  foreach ($envPolicy in $Policy.environments) {
    $name = $envPolicy.name
    $elf = Join-Path (Join-Path $PackageDir $name) 'firmware.elf'
    $bin = Join-Path (Join-Path $PackageDir $name) 'firmware.bin'
    $merged = Join-Path (Join-Path $PackageDir $name) 'firmware-merged.bin'
    if (-not (Test-Path -LiteralPath $elf)) { throw "packaged firmware.elf missing for '$name': $elf" }
    if (-not (Test-Path -LiteralPath $bin)) { throw "packaged firmware.bin missing for '$name': $bin" }
    if (-not (Test-Path -LiteralPath $merged)) { throw "packaged firmware-merged.bin missing for '$name': $merged" }
    $envElf[$name] = $elf; $envBin[$name] = $bin; $envMerged[$name] = $merged
  }
  return (Get-BinaryProofInvariants -Policy $Policy -EnvElf $envElf -EnvBin $envBin -EnvMerged $envMerged -NmPath $nmTool).Invariants
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
  # Strict Windows-hardened package-relative path validator. Rejects (string form):
  #   * null/empty/whitespace;
  #   * absolute paths, drive-qualified (C:\ / C:x), and UNC (\\server) roots;
  #   * ASCII control characters (0x00-0x1F) anywhere (incl. NUL/CR/LF/TAB);
  #   * any empty segment -- leading/trailing separator or repeated separators (// or \\);
  #   * '.' and '..' segments (no-op / traversal);
  #   * ':' in any segment (drive marker / NTFS Alternate Data Stream, e.g. a.txt:evil);
  #   * Windows-illegal / wildcard chars in a segment: < > " | ? * ;
  #   * a segment with a trailing '.' or trailing/leading space (Windows silently strips);
  #   * reserved DOS device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), with or without extension.
  # Callers MUST STILL resolve the joined path and prove it stays inside the package
  # root (Test-PathInside). This validates the textual form only.
  [OutputType([bool])]
  param([Parameter(Mandatory)][AllowEmptyString()][AllowNull()][string]$Rel)
  if ([string]::IsNullOrWhiteSpace($Rel)) { return $false }
  if ($Rel -match '^[\\/]') { return $false }              # leading slash (absolute)
  if ($Rel -match '^[A-Za-z]:') { return $false }          # drive-qualified (C:\ or C:x)
  if ($Rel -match '^[\\/][\\/]') { return $false }         # UNC (\\server)
  foreach ($ch in $Rel.ToCharArray()) { if ([int]$ch -lt 32) { return $false } }  # control chars
  $norm = $Rel -replace '\\', '/'
  $reserved = @('CON', 'PRN', 'AUX', 'NUL',
    'COM1', 'COM2', 'COM3', 'COM4', 'COM5', 'COM6', 'COM7', 'COM8', 'COM9',
    'LPT1', 'LPT2', 'LPT3', 'LPT4', 'LPT5', 'LPT6', 'LPT7', 'LPT8', 'LPT9')
  foreach ($seg in $norm.Split('/')) {
    if ($seg.Length -eq 0) { return $false }                    # empty (repeated/leading/trailing sep)
    if ($seg -eq '.' -or $seg -eq '..') { return $false }       # no-op / traversal
    if ($seg.IndexOf(':') -ge 0) { return $false }              # ADS / drive marker
    if ($seg -match '[<>"|?*]') { return $false }               # illegal / wildcard chars
    if ($seg.EndsWith('.') -or $seg.EndsWith(' ') -or $seg.StartsWith(' ')) { return $false }  # trailing dot/space
    $base = $seg
    $dot = $seg.IndexOf('.')
    if ($dot -ge 0) { $base = $seg.Substring(0, $dot) }
    if ($reserved -contains $base.ToUpperInvariant()) { return $false }  # reserved DOS device name
  }
  return $true
}

# ---------------------------------------------------------------------------
# Release package verification core (shared by verify-release.ps1 and the
# hardware evidence tooling, which re-verifies the release before binding).
# ---------------------------------------------------------------------------

function Test-ReleasePackage {
  # Independently RE-VERIFY an existing release package WITHOUT rebuilding, and
  # REJECT BY DEFAULT unless the package is a fully CERTIFIED, clean, local-only,
  # gate-green build whose manifest / CHECKSUMS / binary-proof are mutually
  # consistent AND whose proof is re-derivable from the packaged binaries.
  # Returns [pscustomobject]@{ Ok; Errors; Checks } and never throws for a normal
  # validation failure (so callers -- including self-tests -- can assert on Ok).
  #
  # $ProofRecomputer is an INTERNAL dependency-injection seam for the tiny
  # self-test fixtures ONLY. When it is $null (the production path used by
  # verify-release and both hardware scripts) the invariants are ALWAYS re-derived
  # from the packaged firmware.elf / firmware.bin with the pinned nm. There is NO
  # public switch that skips the recompute.
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$PackagePath,
    [string]$PolicyPath,
    $Policy,
    [string]$PolicySha256,
    [string]$ReleaseRoot,
    [bool]$RequireUnderReleaseRoot = $true,
    [bool]$RequireHeadMatch = $true,
    [string]$ExpectedHeadCommit,
    [scriptblock]$ProofRecomputer,
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
  function Test-SetsEqual($a, $b) {
    $sa = @(@($a) | Sort-Object); $sb = @(@($b) | Sort-Object)
    if ($sa.Count -ne $sb.Count) { return $false }
    for ($i = 0; $i -lt $sa.Count; $i++) { if ([string]$sa[$i] -ne [string]$sb[$i]) { return $false } }
    return $true
  }

  # Policy is supplied EITHER pre-parsed (production: loaded from the tracked git
  # commit blob, with its exact-bytes SHA-256) OR via -PolicyPath (isolated self-
  # test fixtures ONLY). There is no public working-tree policy override.
  if ($null -eq $Policy) {
    if (-not $PolicyPath) { throw "Test-ReleasePackage requires -Policy (git-loaded) or -PolicyPath (self-test)." }
    $policy = Read-JsonFile $PolicyPath
    if (-not $PolicySha256) { $PolicySha256 = Get-Sha256HexOfBytes ([System.IO.File]::ReadAllBytes($PolicyPath)) }
  } else {
    $policy = $Policy
  }

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

  # Reparse-point defence (fail-closed): lexical containment is NOT enough on
  # Windows. Reject if the package root -- or ANY file/dir under it -- is a
  # junction/symlink/reparse point (which could redirect reads outside the tree).
  $reparseErr = $null; $reparseHit = @()
  try { $reparseHit = @(Get-ContainedReparsePoints $pkgFull) } catch { $reparseErr = $_.Exception.Message }
  if ($reparseErr) {
    Add-Check 'package-no-reparse' $false "reparse-point inspection failed (fail-closed): $reparseErr"
  } else {
    Add-Check 'package-no-reparse' ($reparseHit.Count -eq 0) ("package root/subtree must not be or contain a symlink/junction/reparse point: " + [string]::Join('; ', @($reparseHit | Select-Object -First 3)))
  }

  # -- 2. Required files + load manifest/proof --------------------------------
  $manifestPath = Join-Path $pkgFull 'manifest.json'
  $checksumsPath = Join-Path $pkgFull 'CHECKSUMS.sha256'
  $proofPath = Join-Path $pkgFull 'binary-proof.json'
  $proofTxtPath = Join-Path $pkgFull 'binary-proof.txt'
  foreach ($req in @($manifestPath, $checksumsPath, $proofPath, $proofTxtPath)) {
    if (-not (Test-Path $req)) {
      Add-Check 'required-file' $false "missing $req"
      return [pscustomobject]@{ Ok = $false; Errors = $errors; Checks = $checks }
    }
  }
  $manifest = Read-JsonFile $manifestPath
  $proof = Read-JsonFile $proofPath

  Add-Check 'manifest-schema' (([string](Get-PsObjectProperty $manifest 'schema')) -eq 'plane-radar/release-manifest') "unexpected manifest schema"
  Add-Check 'manifest-schema-version' ([int](Get-PsObjectProperty $manifest 'schemaVersion') -eq 1) "manifest schemaVersion must be 1"
  Add-Check 'proof-schema' (([string](Get-PsObjectProperty $proof 'schema')) -eq 'plane-radar/binary-proof') "unexpected binary-proof schema"
  Add-Check 'proof-schema-version' ([int](Get-PsObjectProperty $proof 'schemaVersion') -eq 1) "binary-proof schemaVersion must be 1"

  # -- 2b. Tracked-git policy binding: manifest.policy_sha256 must equal the -----
  # SHA-256 of the EXACT git-blob release-policy.json bytes used for verification.
  $manPolicySha = [string](Get-PsObjectProperty $manifest 'policy_sha256')
  if ($PolicySha256) {
    Add-Check 'manifest-policy-sha' ((Test-Sha256Hex $manPolicySha) -and ($manPolicySha -eq $PolicySha256)) "manifest.policy_sha256 must equal the SHA-256 of the tracked git-blob release-policy.json used ($PolicySha256)"
  } else {
    Add-Check 'manifest-policy-sha' (Test-Sha256Hex $manPolicySha) "manifest.policy_sha256 must be a 64-hex SHA-256"
  }

  # -- 3. Commit / path binding ----------------------------------------------
  $manifestCommit = [string](Get-PsObjectProperty (Get-PsObjectProperty $manifest 'git') 'commit')
  $boundCommit = $ExpectedHeadCommit
  if ($RequireUnderReleaseRoot) { $boundCommit = $leaf }
  Add-Check 'manifest-commit-binding' ($manifestCommit -eq $boundCommit -and (Test-Sha1Hex $manifestCommit)) "manifest commit '$manifestCommit' must equal package directory '$boundCommit'"
  if ($RequireHeadMatch) {
    Add-Check 'head-match' ($manifestCommit -eq $ExpectedHeadCommit) "manifest commit '$manifestCommit' must equal current HEAD '$ExpectedHeadCommit' (use -AllowStaleHead for an archived, still-certified package)"
  }
  Add-Check 'proof-commit-binding' (([string](Get-PsObjectProperty $proof 'commit')) -eq $manifestCommit) "binary-proof commit must equal manifest commit"

  # -- 4. Certification: never accept a non-certified / dirty build -----------
  $build = Get-PsObjectProperty $manifest 'build'
  Add-Check 'build-certified' ([bool](Get-PsObjectProperty $build 'certified')) "manifest build.certified must be true (diagnostic / non-certified packages are rejected)"
  Add-Check 'build-clean' ([bool](Get-PsObjectProperty $build 'clean_build')) "manifest build.clean_build must be true"

  # -- 5. Local-only / clean-worktree binding ---------------------------------
  $git = Get-PsObjectProperty $manifest 'git'
  Add-Check 'git-tracked-clean' ([bool](Get-PsObjectProperty $git 'tracked_clean')) "manifest git.tracked_clean must be true"
  $untracked = [int](Get-PsObjectProperty $git 'untracked_files')
  Add-Check 'git-untracked-zero' ($untracked -eq 0) "manifest git.untracked_files must be 0 (got $untracked)"
  $localOnly = Get-PsObjectProperty $manifest 'local_only'
  Add-Check 'git-no-remote' ([bool](Get-PsObjectProperty $localOnly 'no_remote')) "manifest local_only.no_remote must be true"
  Add-Check 'local-only-release-path' (([string](Get-PsObjectProperty $localOnly 'release_path')) -eq "release/$manifestCommit") "manifest local_only.release_path must be release/<commit>"

  # -- 6. Source gates: exact duplicate-free set, all passed ------------------
  $policyGateIds = @($policy.required_source_gates | ForEach-Object { [string]$_.id })
  $policyGateScript = @{}; foreach ($g in $policy.required_source_gates) { $policyGateScript[[string]$g.id] = [string]$g.script }
  $manifestGates = @(Get-PsObjectProperty $manifest 'gates')
  $manifestGateIds = @($manifestGates | ForEach-Object { [string](Get-PsObjectProperty $_ 'id') })
  $gateDupFree = ($manifestGateIds.Count -eq (@($manifestGateIds | Select-Object -Unique).Count))
  Add-Check 'gates-exact-set' ($gateDupFree -and (Test-SetsEqual $policyGateIds $manifestGateIds)) "manifest gates must be the exact duplicate-free set of policy required_source_gates"
  $gateStatusOk = ($manifestGates.Count -gt 0)
  foreach ($mg in $manifestGates) {
    $gid = [string](Get-PsObjectProperty $mg 'id')
    if (([string](Get-PsObjectProperty $mg 'status')) -ne 'passed') { $gateStatusOk = $false }
    if (-not $policyGateScript.ContainsKey($gid)) { $gateStatusOk = $false }
    elseif (([string](Get-PsObjectProperty $mg 'script')) -ne $policyGateScript[$gid]) { $gateStatusOk = $false }
  }
  Add-Check 'gates-all-passed' $gateStatusOk "every manifest gate must have status 'passed' and match the policy gate script"

  $tests = Get-PsObjectProperty $manifest 'tests'
  Add-Check 'tests-gates-run' ([bool](Get-PsObjectProperty $tests 'gates_run')) "manifest tests.gates_run must be true"
  $policyTests = Get-PsObjectProperty $policy 'tests'
  $expNativeSummary = [string](Get-PsObjectProperty $policyTests 'native_test_summary')
  Add-Check 'tests-native-summary' (($expNativeSummary.Length -gt 0) -and (([string](Get-PsObjectProperty $tests 'native_test')) -eq $expNativeSummary)) "manifest tests.native_test must equal policy tests.native_test_summary"

  # -- 7. Pins: expected == policy AND observed (measured) == policy ----------
  $pins = Get-PsObjectProperty $manifest 'pins'
  $expPins = $policy.pins
  $pinExpectedOk = $true
  foreach ($fld in @('platformio_core', 'platform', 'framework', 'framework_arduinoespressif32', 'toolchain_riscv32_esp', 'tool_esptoolpy', 'board', 'mcu', 'flash_size', 'app_offset')) {
    if (([string](Get-PsObjectProperty $pins $fld)) -ne ([string](Get-PsObjectProperty $expPins $fld))) { $pinExpectedOk = $false }
  }
  # platformio_core_verified is the EXACT `pio --version` banner string proven at
  # build time; it must match "PlatformIO Core, version <pin>" exactly.
  $expPioVerified = "PlatformIO Core, version " + [string]$expPins.platformio_core
  if (([string](Get-PsObjectProperty $pins 'platformio_core_verified')) -ne $expPioVerified) { $pinExpectedOk = $false }
  $manDeps = Get-PsObjectProperty $pins 'dependencies'
  foreach ($dp in $expPins.dependencies.PSObject.Properties) {
    if (([string](Get-PsObjectProperty $manDeps $dp.Name)) -ne [string]$dp.Value) { $pinExpectedOk = $false }
  }
  Add-Check 'pins-expected-match-policy' $pinExpectedOk "manifest expected pins (core/verified banner/platform/framework/framework-pkg/toolchain/esptool/board/mcu/flash_size/app_offset + dependencies) must equal policy pins"

  $obs = Get-PsObjectProperty $pins 'observed'
  $obsOk = ($null -ne $obs)
  if ($obsOk) {
    $expPlatformVer = Get-PolicyPlatformVersion ([string]$expPins.platform)
    if (([string](Get-PsObjectProperty $obs 'platform_version')) -ne $expPlatformVer) { $obsOk = $false }
    foreach ($fld in @('framework_arduinoespressif32', 'toolchain_riscv32_esp', 'tool_esptoolpy')) {
      if (([string](Get-PsObjectProperty $obs $fld)) -ne ([string](Get-PsObjectProperty $expPins $fld))) { $obsOk = $false }
    }
    $obsDeps = Get-PsObjectProperty $obs 'dependencies'
    foreach ($dp in $expPins.dependencies.PSObject.Properties) {
      if (([string](Get-PsObjectProperty $obsDeps $dp.Name)) -ne [string]$dp.Value) { $obsOk = $false }
    }
  }
  Add-Check 'pins-observed-match-policy' $obsOk "manifest pins.observed (measured installed versions) must be present and equal the policy pins"
  Add-Check 'airport-source-commit' (([string](Get-PsObjectProperty $manifest 'airport_source_commit')) -eq [string]$policy.airport_data.source_commit) "manifest airport_source_commit must equal policy airport_data.source_commit"

  # -- 8. Artifacts: exact keys, per-artifact size + hash + safe path ---------
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
    # env ownership: '<env>/...' belongs to <env>; root proof files own env ''.
    $expOwner = ''
    if ($rel.Contains('/')) { $expOwner = $rel.Split('/')[0] }
    $ownerOk = (([string](Get-PsObjectProperty $p.Value 'env')) -eq $expOwner)
    if (-not ($sizeOk -and $hashOk -and $ownerOk)) { $artifactBad++ }
  }
  Add-Check 'manifest-artifacts' ($artifactBad -eq 0) "$artifactBad artifact(s) failed size/hash/path/owner validation"

  # Exact key set: EXACTLY the per-env copied artifacts + binary-proof.json/txt,
  # AND exactly the actual package files (excluding manifest/CHECKSUMS + the
  # post-build hardware-evidence/ subtree). Rejects unlisted extras and
  # manifest-listed-but-missing files.
  $manifestKeys = @($artifactProps | ForEach-Object { $_.Name })
  $expectedKeys = New-Object System.Collections.Generic.List[string]
  foreach ($envPolicy in $policy.environments) {
    foreach ($cn in $policy.copied_artifacts) { $expectedKeys.Add("$($envPolicy.name)/$cn") }
  }
  $expectedKeys.Add('binary-proof.json'); $expectedKeys.Add('binary-proof.txt')
  Add-Check 'artifact-keys-expected' (Test-SetsEqual $expectedKeys $manifestKeys) "manifest artifact keys must be exactly the per-env copied artifacts + binary-proof.json/txt"

  $artifactActual = New-Object System.Collections.Generic.List[string]
  foreach ($f in (Get-ChildItem -LiteralPath $pkgFull -Recurse -File)) {
    $rel = Get-PackageRelPath -Root $pkgFull -FullPath $f.FullName
    if ($rel -eq 'manifest.json' -or $rel -eq 'CHECKSUMS.sha256') { continue }
    if ($rel -eq 'hardware-evidence' -or $rel -like 'hardware-evidence/*') { continue }
    $artifactActual.Add($rel)
  }
  Add-Check 'artifact-keys-match-files' (Test-SetsEqual $artifactActual $manifestKeys) "manifest artifact keys must exactly match the actual package files (no unlisted extras / missing)"

  # -- 9. Default artifact identity + role + size + hash binding --------------
  $defExpected = $policy.default_artifact.relative_path
  $defManifest = Get-PsObjectProperty $manifest 'default_artifact'
  $defRel = [string](Get-PsObjectProperty $defManifest 'relative_path')
  Add-Check 'default-artifact-path' ($defRel -eq $defExpected) "default artifact must be '$defExpected' (got '$defRel')"
  Add-Check 'default-artifact-role' (([string](Get-PsObjectProperty $defManifest 'role')) -eq 'default-release') "default artifact role must be default-release"
  Add-Check 'default-artifact-env' (([string](Get-PsObjectProperty $defManifest 'environment')) -eq [string]$policy.default_artifact.environment) "default artifact environment must equal policy"
  $defArtProp = $artifacts.PSObject.Properties[$defRel]
  if ($defArtProp) {
    $defHashOk = (([string](Get-PsObjectProperty $defManifest 'sha256')) -eq [string]$defArtProp.Value.sha256)
    $defSizeOk = ([int64](Get-PsObjectProperty $defManifest 'size') -eq [int64]$defArtProp.Value.size)
    Add-Check 'default-artifact-hash-binding' $defHashOk "default_artifact sha256 must match its artifacts entry"
    Add-Check 'default-artifact-size-binding' $defSizeOk "default_artifact size must match its artifacts entry"
  } else {
    Add-Check 'default-artifact-in-artifacts' $false "default artifact '$defRel' not in artifacts map"
  }

  # -- 10. Environments: exact duplicate-free set + role/eval/options/resources
  #        + exact policy SHA-256 anchors + nested-artifact consistency ---------
  $manifestEnvArray = @(Get-PsObjectProperty $manifest 'environments')
  $manifestEnvNames = @($manifestEnvArray | ForEach-Object { [string](Get-PsObjectProperty $_ 'name') })
  $envDupFree = ($manifestEnvNames.Count -eq (@($manifestEnvNames | Select-Object -Unique).Count))
  $envByName = @{}
  foreach ($e in $manifestEnvArray) { $envByName[[string](Get-PsObjectProperty $e 'name')] = $e }
  $policyEnvNames = @($policy.environments | ForEach-Object { [string]$_.name })
  Add-Check 'env-exact-set' ($envDupFree -and ($manifestEnvNames.Count -eq $policyEnvNames.Count) -and (Test-SetsEqual $policyEnvNames $manifestEnvNames)) "manifest environments must be the EXACT duplicate-free set of policy environments (no extra/missing/duplicate)"
  foreach ($envPolicy in $policy.environments) {
    $name = $envPolicy.name
    if (-not $envByName.ContainsKey($name)) { Add-Check "env/$name" $false "environment missing from manifest"; continue }
    $me = $envByName[$name]
    $res = Get-PsObjectProperty $me 'resources'
    $exp = $envPolicy.resources
    $okBin = ([int64](Get-PsObjectProperty $res 'firmware_bin') -eq [int64]$exp.firmware_bin)
    $okMerged = ([int64](Get-PsObjectProperty $res 'merged_bin') -eq [int64]$exp.merged_bin)
    $rRam = Get-PsObjectProperty $res 'ram'; $rFlash = Get-PsObjectProperty $res 'flash'
    $okRam = (($null -ne $rRam) -and ([int64]$rRam -eq [int64]$exp.ram))
    $okFlash = (($null -ne $rFlash) -and ([int64]$rFlash -eq [int64]$exp.flash))
    $binArt = $artifacts.PSObject.Properties["$name/firmware.bin"]
    $mergedArt = $artifacts.PSObject.Properties["$name/firmware-merged.bin"]
    $consistent = ($binArt -and $mergedArt -and ([int64]$binArt.Value.size -eq [int64](Get-PsObjectProperty $res 'firmware_bin')) -and ([int64]$mergedArt.Value.size -eq [int64](Get-PsObjectProperty $res 'merged_bin')))
    Add-Check "resource-policy/$name" ($okBin -and $okMerged -and $okRam -and $okFlash -and $consistent) `
      "resources must equal policy (ram=$($exp.ram),flash=$($exp.flash),bin=$($exp.firmware_bin),merged=$($exp.merged_bin)) and match copied artifacts"

    # EXACT policy SHA-256 anchor: the packaged firmware.bin AND the SHIPPED
    # firmware-merged.bin must equal the tracked policy hashes (external anchor,
    # NOT manifest self-consistency). This reads firmware-merged.bin directly.
    $polSha = Get-PsObjectProperty $envPolicy 'sha256'
    $expFwSha = ''; $expMgSha = ''
    if ($polSha) { $expFwSha = [string](Get-PsObjectProperty $polSha 'firmware_bin'); $expMgSha = [string](Get-PsObjectProperty $polSha 'merged_bin') }
    $binFile = Join-Path $pkgFull "$name\firmware.bin"
    $mergedFile = Join-Path $pkgFull "$name\firmware-merged.bin"
    $fwShaOk = ((Test-Path $binFile) -and ($expFwSha.Length -eq 64) -and ((Get-Sha256Hex $binFile) -eq $expFwSha))
    $mgShaOk = ((Test-Path $mergedFile) -and ($expMgSha.Length -eq 64) -and ((Get-Sha256Hex $mergedFile) -eq $expMgSha))
    Add-Check "image-sha-policy/$name" ($fwShaOk -and $mgShaOk) "packaged firmware.bin and firmware-merged.bin must equal the EXACT policy SHA-256 anchors"

    # Nested environment.artifacts: exact copied_artifacts set whose size/SHA/env
    # equal the flat artifact map entry for '<env>/<name>'. A forged nested hash
    # MUST fail here.
    $nested = Get-PsObjectProperty $me 'artifacts'
    $nestedProps = @(); if ($nested) { $nestedProps = @($nested.PSObject.Properties) }
    $nestedNames = @($nestedProps | ForEach-Object { $_.Name })
    $nestedDupFree = ($nestedNames.Count -eq (@($nestedNames | Select-Object -Unique).Count))
    $nestedSetOk = ($nestedDupFree -and (Test-SetsEqual @($policy.copied_artifacts) $nestedNames))
    $nestedConsistent = $nestedSetOk
    foreach ($np in $nestedProps) {
      $flatKey = "$name/$($np.Name)"
      $flatProp = $artifacts.PSObject.Properties[$flatKey]
      if (-not $flatProp) { $nestedConsistent = $false; continue }
      if (([int64](Get-PsObjectProperty $np.Value 'size')) -ne [int64]$flatProp.Value.size) { $nestedConsistent = $false }
      if (([string](Get-PsObjectProperty $np.Value 'sha256')) -ne [string]$flatProp.Value.sha256) { $nestedConsistent = $false }
      if (([string](Get-PsObjectProperty $np.Value 'env')) -ne $name) { $nestedConsistent = $false }
    }
    Add-Check "env-nested-artifacts/$name" ($nestedSetOk -and $nestedConsistent) "nested environment.artifacts must be the exact copied_artifacts set with size/SHA/env equal to the flat artifact map (no forged nested hash)"

    Add-Check "env-role/$name" (([string](Get-PsObjectProperty $me 'role')) -eq [string]$envPolicy.role) "role must be '$($envPolicy.role)'"
    Add-Check "env-eval/$name" (([bool](Get-PsObjectProperty $me 'evaluation_only')) -eq [bool]$envPolicy.evaluation_only) "evaluation_only must be $([bool]$envPolicy.evaluation_only)"
    $mo = Get-PsObjectProperty $me 'options'; $po = $envPolicy.options
    $optOk = ((([bool](Get-PsObjectProperty $mo 'worker')) -eq [bool]$po.worker) -and `
              (([bool](Get-PsObjectProperty $mo 'diagnostics')) -eq [bool]$po.diagnostics) -and `
              ([int](Get-PsObjectProperty $mo 'log_level') -eq [int]$po.log_level))
    Add-Check "env-options/$name" $optOk "options must equal policy (worker=$($po.worker),diagnostics=$($po.diagnostics),log_level=$($po.log_level))"
  }

  # -- 11. CHECKSUMS: recompute, exact closed coverage ------------------------
  $checksumText = Get-Content -Raw -LiteralPath $checksumsPath
  $checksumLines = @(($checksumText -replace "`r`n", "`n") -split "`n" | Where-Object { $_.Trim().Length -gt 0 })
  $covered = @{}
  $dupCount = 0; $badLine = 0; $hashMismatch = 0; $traversal = 0
  foreach ($line in $checksumLines) {
    $m = [regex]::Match($line, '^(?<hash>[0-9a-f]{64})  (?<path>.+)$')
    if (-not $m.Success) { $badLine++; continue }
    $rel = $m.Groups['path'].Value
    if (-not (Test-RelPathSafe $rel)) { $traversal++; continue }
    if ($rel -eq 'CHECKSUMS.sha256') { $badLine++; continue }
    if ($covered.ContainsKey($rel)) { $dupCount++; continue }
    $covered[$rel] = $true
    $file = Join-Path $pkgFull ($rel -replace '/', '\')
    if (-not (Test-PathInside -Base $pkgFull -Candidate $file)) { $traversal++; continue }
    if (-not (Test-Path $file)) { $badLine++; continue }
    if ((Get-Sha256Hex $file) -ne $m.Groups['hash'].Value) { $hashMismatch++ }
  }
  Add-Check 'checksums-parse' ($badLine -eq 0) "$badLine malformed/dangling CHECKSUMS line(s)"
  Add-Check 'checksums-no-duplicate' ($dupCount -eq 0) "$dupCount duplicate path(s)"
  Add-Check 'checksums-no-traversal' ($traversal -eq 0) "$traversal traversal/absolute/unsafe path(s)"
  Add-Check 'checksums-hash-match' ($hashMismatch -eq 0) "$hashMismatch checksum hash mismatch(es)"

  $checksumActual = @{}
  foreach ($f in (Get-ChildItem -LiteralPath $pkgFull -Recurse -File)) {
    $rel = Get-PackageRelPath -Root $pkgFull -FullPath $f.FullName
    if ($rel -eq 'CHECKSUMS.sha256') { continue }
    if ($rel -eq 'hardware-evidence' -or $rel -like 'hardware-evidence/*') { continue }
    $checksumActual[$rel] = $true
  }
  $missing = @($checksumActual.Keys | Where-Object { -not $covered.ContainsKey($_) })
  $extra = @($covered.Keys | Where-Object { -not $checksumActual.ContainsKey($_) })
  Add-Check 'checksums-covers-all' ($missing.Count -eq 0) "$($missing.Count) file(s) not covered: $([string]::Join(', ', @($missing | Select-Object -First 5)))"
  Add-Check 'checksums-no-extra' ($extra.Count -eq 0) "$($extra.Count) checksum entry(ies) without a file: $([string]::Join(', ', @($extra | Select-Object -First 5)))"
  Add-Check 'checksums-covers-manifest' ($covered.ContainsKey('manifest.json')) "manifest.json must be covered by CHECKSUMS"

  # -- 12. Binary proof: RE-DERIVE from packaged binaries + cross-check -------
  $declInvariants = @(Get-PsObjectProperty $proof 'invariants')
  $recomputed = $null; $recomputeErr = $null
  try {
    if ($ProofRecomputer) { $recomputed = @(& $ProofRecomputer $pkgFull $policy) }
    else { $recomputed = @(Get-PackageDerivedInvariants -PackageDir $pkgFull -Policy $policy) }
  } catch { $recomputeErr = $_.Exception.Message }

  if ($recomputeErr) {
    Add-Check 'proof-recompute' $false "could not re-derive proof from packaged binaries: $recomputeErr"
  } else {
    $declById = @{}; foreach ($d in $declInvariants) { $declById[[string](Get-InvariantField $d 'id')] = $d }
    $recById = @{}; foreach ($r in $recomputed) { $recById[[string](Get-InvariantField $r 'id')] = $r }
    $idSetOk = ($declInvariants.Count -eq $recomputed.Count) -and ($declById.Count -eq $declInvariants.Count) -and ($recById.Count -eq $recomputed.Count)
    if ($idSetOk) {
      foreach ($k in $recById.Keys) { if (-not $declById.ContainsKey($k)) { $idSetOk = $false } }
      foreach ($k in $declById.Keys) { if (-not $recById.ContainsKey($k)) { $idSetOk = $false } }
    }
    Add-Check 'proof-invariant-id-set' $idSetOk "declared binary-proof invariants must be EXACTLY the re-derived set (no extra/trimmed/duplicate ids)"

    $fieldsOk = $idSetOk
    if ($idSetOk) {
      foreach ($k in $recById.Keys) {
        $r = $recById[$k]; $d = $declById[$k]
        foreach ($fld in @('category', 'environment', 'expectation', 'observed', 'status')) {
          if (([string](Get-InvariantField $r $fld)) -ne ([string](Get-InvariantField $d $fld))) { $fieldsOk = $false }
        }
      }
    }
    Add-Check 'proof-invariant-fields' $fieldsOk "each declared invariant must match the re-derived category/environment/expectation/observed/status (no forged/contradictory record)"

    $recOverall = Get-ProofOverall $recomputed
    Add-Check 'proof-recompute-pass' ($recOverall -eq 'pass') "re-derived proof must have EVERY invariant pass (got overall '$recOverall')"
    $declOverall = [string](Get-PsObjectProperty $proof 'overall')
    Add-Check 'proof-overall-consistent' (($declOverall -eq $recOverall) -and ($declOverall -eq 'pass')) "declared binary-proof.overall must equal the re-derived overall and be pass"

    $recCount = $recomputed.Count
    $recPass = @($recomputed | Where-Object { ([string](Get-InvariantField $_ 'status')) -eq 'pass' }).Count
    $recFail = $recCount - $recPass
    $totalsOk = (([int](Get-PsObjectProperty $proof 'total') -eq $recCount) -and ([int](Get-PsObjectProperty $proof 'passed') -eq $recPass) -and ([int](Get-PsObjectProperty $proof 'failed') -eq $recFail))
    Add-Check 'proof-totals' $totalsOk "declared proof total/passed/failed must equal the re-derived totals"

    $recCats = Get-ProofCategoryCounts $recomputed
    $declCats = Get-PsObjectProperty $proof 'categories'
    $catsOk = $true
    $declCatNames = @(); if ($declCats) { $declCatNames = @($declCats.PSObject.Properties | ForEach-Object { $_.Name }) }
    foreach ($ck in $recCats.Keys) { if (([string](Get-PsObjectProperty $declCats $ck)) -ne ([string]$recCats[$ck])) { $catsOk = $false } }
    if ($declCatNames.Count -ne $recCats.Count) { $catsOk = $false }
    Add-Check 'proof-categories' $catsOk "declared proof categories must equal the re-derived category counts"

    $psum = Get-PsObjectProperty $manifest 'proof_summary'
    $psumOk = ((([string](Get-PsObjectProperty $psum 'overall')) -eq 'pass') -and `
      ([int](Get-PsObjectProperty $psum 'total') -eq $recCount) -and `
      ([int](Get-PsObjectProperty $psum 'passed') -eq $recPass) -and `
      ([int](Get-PsObjectProperty $psum 'failed') -eq $recFail))
    Add-Check 'manifest-proof-summary' $psumOk "manifest.proof_summary must be pass and match the re-derived totals"
  }

  return [pscustomobject]@{ Ok = ($errors.Count -eq 0); Errors = $errors; Checks = $checks }
}

function Get-FixtureProofInvariants {
  # The canonical, file-independent invariant set for the self-test fixtures. It
  # is written into the fixture binary-proof.json AND returned by the injected
  # ProofRecomputer stub, so a well-formed fixture matches and any forged/trimmed/
  # contradictory proof edit is detected by the recompute cross-check.
  [OutputType([object[]])]
  param([string]$EnvName = 'supermini')
  return @(
    (New-InvariantResult -Id "fixture/$EnvName/present" -Category 'fixture-present' -Environment $EnvName -Description 'fixture present invariant' -Expectation 'present' -Observed 'present' -Pass $true),
    (New-InvariantResult -Id "fixture/$EnvName/absent" -Category 'fixture-absent' -Environment $EnvName -Description 'fixture absent invariant' -Expectation 'absent' -Observed 'absent' -Pass $true)
  )
}

function New-MinimalReleasePackage {
  # Build a SMALL but fully CERTIFICATION-CONFORMANT release package in $Dir for
  # self-tests ONLY (never touches real firmware, never writes under release/).
  # It satisfies every strict Test-ReleasePackage check when paired with the
  # merged-layout-aware ProofRecomputer stub. The fixture merged image has a REAL
  # (tiny) component/gap/app layout so the merged-layout proof exercises real code.
  # The fixture policy is written OUTSIDE the package directory so it does not
  # pollute the artifact set.
  [OutputType([hashtable])]
  param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Commit,
    [string]$EnvName = 'supermini'
  )
  New-Item -ItemType Directory -Path $Dir -Force | Out-Null
  $envDir = Join-Path $Dir $EnvName
  New-Item -ItemType Directory -Path $envDir -Force | Out-Null

  $copied = @('firmware.bin', 'firmware-merged.bin', 'firmware.elf', 'firmware.map', 'build.log', 'merge.log', 'nm-symbols.txt')

  # -- Realistic tiny merged layout ------------------------------------------
  #   bootloader@0(8) | gap(8) | partitions@16(8) | gap(8) | boot_app0@32(8) |
  #   gap(8) | app@48(32).  merged length = app_offset(48) + firmware.bin(32) = 80.
  $fillByte = [byte]255
  $binBytes = [byte[]](1..32)                       # firmware.bin (the app)
  $compBoot = [byte[]](100..107)
  $compPart = [byte[]](110..117)
  $compApp0 = [byte[]](120..127)
  $appOffset = 48
  $mergedList = New-Object System.Collections.Generic.List[byte]
  $mergedList.AddRange($compBoot)
  for ($i = 0; $i -lt 8; $i++) { $mergedList.Add($fillByte) }
  $mergedList.AddRange($compPart)
  for ($i = 0; $i -lt 8; $i++) { $mergedList.Add($fillByte) }
  $mergedList.AddRange($compApp0)
  for ($i = 0; $i -lt 8; $i++) { $mergedList.Add($fillByte) }
  $mergedList.AddRange($binBytes)
  $mergedBytes = $mergedList.ToArray()

  [System.IO.File]::WriteAllBytes((Join-Path $envDir 'firmware.bin'), $binBytes)
  [System.IO.File]::WriteAllBytes((Join-Path $envDir 'firmware-merged.bin'), $mergedBytes)
  Write-TextFileLf (Join-Path $envDir 'firmware.elf') "fake-elf"
  Write-TextFileLf (Join-Path $envDir 'firmware.map') "fake-map"
  Write-TextFileLf (Join-Path $envDir 'build.log') "fake-build"
  Write-TextFileLf (Join-Path $envDir 'merge.log') "fake-merge"
  Write-TextFileLf (Join-Path $envDir 'nm-symbols.txt') "fake-nm"

  $defRel = "$EnvName/firmware-merged.bin"
  $deps = [ordered]@{ 'lovyan03/LovyanGFX' = '9.9.9'; 'bblanchon/ArduinoJson' = '8.8.8' }
  $fwSha = Get-Sha256HexOfBytes $binBytes
  $mgSha = Get-Sha256HexOfBytes $mergedBytes
  $pinsCommon = [ordered]@{
    platformio_core = '0.0.0'; platform = 'fxplatform@1.2.3'; framework = 'arduino'
    framework_arduinoespressif32 = 'fx-fw'; toolchain_riscv32_esp = 'fx-tc'; tool_esptoolpy = 'fx-esptool'
    board = 'fxboard'; mcu = 'fxmcu'; flash_size = '4MB'; app_offset = '0x10000'
    dependencies = $deps
  }
  $fxPolicy = [ordered]@{
    schema = 'plane-radar/release-policy'; schema_version = 1
    local_only = [ordered]@{ require_no_git_remote = $true; release_root_relative = 'release' }
    pins = [ordered]@{
      platformio_core = '0.0.0'; platform = 'fxplatform@1.2.3'; framework = 'arduino'
      framework_arduinoespressif32 = 'fx-fw'; toolchain_riscv32_esp = 'fx-tc'; tool_esptoolpy = 'fx-esptool'
      board = 'fxboard'; mcu = 'fxmcu'; flash_size = '4MB'; app_offset = '0x10000'
      dependencies = $deps
    }
    airport_data = [ordered]@{ source_commit = 'fxairportcommit0000000000000000000000000' }
    default_artifact = [ordered]@{ environment = $EnvName; relative_path = $defRel; role = 'default-release' }
    required_source_gates = @(
      [ordered]@{ id = 'fx-gate-a'; script = 'scripts/fx-a.ps1'; summary = 'fa'; args = @() },
      [ordered]@{ id = 'fx-gate-b'; script = 'scripts/fx-b.ps1'; summary = 'fb'; args = @() }
    )
    tests = [ordered]@{ native_test_summary = 'fixture native summary' }
    merged_layout = [ordered]@{
      app_offset = $appOffset; gap_fill_byte = 255
      components = @(
        [ordered]@{ name = 'bootloader'; offset = 0;  size = 8; sha256 = (Get-Sha256HexOfBytes $compBoot) },
        [ordered]@{ name = 'partitions'; offset = 16; size = 8; sha256 = (Get-Sha256HexOfBytes $compPart) },
        [ordered]@{ name = 'boot_app0';  offset = 32; size = 8; sha256 = (Get-Sha256HexOfBytes $compApp0) }
      )
    }
    copied_artifacts = $copied
    environments = @([ordered]@{
        name = $EnvName; role = 'default-release'; evaluation_only = $false
        options = [ordered]@{ worker = $false; diagnostics = $false; log_level = 2 }
        resources = [ordered]@{ ram = 1; flash = 2; firmware_bin = $binBytes.Length; merged_bin = $mergedBytes.Length }
        sha256 = [ordered]@{ firmware_bin = $fwSha; merged_bin = $mgSha }
      })
  }
  $policyPath = (Get-FullPathSafe $Dir).TrimEnd('\', '/') + '-policy.json'
  Write-JsonFileLf $policyPath $fxPolicy
  $policySha = Get-Sha256Hex $policyPath
  $fxPolicyObj = Read-JsonFile $policyPath

  $fxInvariants = @(Get-FixtureProofInvariants -EnvName $EnvName)
  $fxInvariants += @(Get-MergedLayoutInvariants -Policy $fxPolicyObj -Name $EnvName `
    -MergedPath (Join-Path $envDir 'firmware-merged.bin') -FirmwarePath (Join-Path $envDir 'firmware.bin'))
  $proof = [ordered]@{
    schema = 'plane-radar/binary-proof'; schemaVersion = 1; commit = $Commit
    overall = 'pass'; total = $fxInvariants.Count; passed = $fxInvariants.Count; failed = 0
    categories = (Get-ProofCategoryCounts $fxInvariants)
    invariants = $fxInvariants
  }
  Write-JsonFileLf (Join-Path $Dir 'binary-proof.json') $proof
  Write-TextFileLf (Join-Path $Dir 'binary-proof.txt') "fixture proof"

  $artifactsMap = New-PackageArtifactsMap -PackageDir $Dir
  $mergedSha = $artifactsMap[$defRel].sha256
  $nestedArtifacts = [ordered]@{}
  foreach ($rel in $artifactsMap.Keys) {
    if ($rel.StartsWith("$EnvName/")) { $nestedArtifacts[$rel.Substring($EnvName.Length + 1)] = $artifactsMap[$rel] }
  }
  $manifest = [ordered]@{
    schema = 'plane-radar/release-manifest'; schemaVersion = 1
    git = [ordered]@{ commit = $Commit; short = $Commit.Substring(0, 7); branch = 'fixture'; tracked_clean = $true; untracked_files = 0 }
    local_only = [ordered]@{ no_remote = $true; release_path = "release/$Commit" }
    policy_sha256 = $policySha
    pins = [ordered]@{
      platformio_core = '0.0.0'; platformio_core_verified = 'PlatformIO Core, version 0.0.0'
      platform = 'fxplatform@1.2.3'; framework = 'arduino'
      framework_arduinoespressif32 = 'fx-fw'; toolchain_riscv32_esp = 'fx-tc'; tool_esptoolpy = 'fx-esptool'
      board = 'fxboard'; mcu = 'fxmcu'; flash_size = '4MB'; app_offset = '0x10000'
      dependencies = $deps
      observed = [ordered]@{
        platform_version = '1.2.3'; framework_arduinoespressif32 = 'fx-fw'
        toolchain_riscv32_esp = 'fx-tc'; tool_esptoolpy = 'fx-esptool'; dependencies = $deps
      }
    }
    airport_source_commit = 'fxairportcommit0000000000000000000000000'
    default_artifact = [ordered]@{ environment = $EnvName; relative_path = $defRel; role = 'default-release'; size = $mergedBytes.Length; sha256 = $mergedSha }
    gates = @(
      [ordered]@{ id = 'fx-gate-a'; script = 'scripts/fx-a.ps1'; status = 'passed'; summary = 'fa' },
      [ordered]@{ id = 'fx-gate-b'; script = 'scripts/fx-b.ps1'; status = 'passed'; summary = 'fb' }
    )
    tests = [ordered]@{ native_test = 'fixture native summary'; gates_run = $true }
    build = [ordered]@{ clean_build = $true; certified = $true }
    proof_summary = [ordered]@{ overall = 'pass'; total = $fxInvariants.Count; passed = $fxInvariants.Count; failed = 0 }
    environments = @([ordered]@{
        name = $EnvName; role = 'default-release'; evaluation_only = $false
        options = [ordered]@{ worker = $false; diagnostics = $false; log_level = 2 }
        resources = [ordered]@{ ram = 1; flash = 2; firmware_bin = $binBytes.Length; merged_bin = $mergedBytes.Length }
        artifacts = $nestedArtifacts
      })
    artifacts = $artifactsMap
  }
  Write-JsonFileLf (Join-Path $Dir 'manifest.json') $manifest
  Write-PackageChecksums -PackageDir $Dir

  return @{
    PolicyPath  = $policyPath
    PolicySha256 = $policySha
    DefaultRel  = $defRel
    ImageSize   = [int64]$mergedBytes.Length
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
    [Parameter(Mandatory)][string]$GeneratedUtc,
    [string]$PolicySha256 = ''
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
      # size=null / sha256='' is the PENDING scaffold. For a pass the operator
      # records the real file's byte length + full lowercase SHA-256, which the
      # verifier re-computes and binds. There is no editable 'present' authority.
      $ev.Add([ordered]@{ name = $e.name; description = $e.description; size = $null; sha256 = '' })
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
    policy_sha256  = $PolicySha256
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
  # strings so 'yes'/'no'/'pass' and numeric-as-string both work. 'between' takes
  # a two-element [lo, hi] Expected and passes iff lo <= measured <= hi (used to
  # reject "too early" observations that a bare <= would accept). A null (never
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
    'between' {
      $arr = @($Expected)
      if ($arr.Count -ne 2) { return $false }
      $m = 0.0; $lo = 0.0; $hi = 0.0
      if (-not [double]::TryParse("$Measured", [ref]$m)) { return $false }
      if (-not [double]::TryParse("$($arr[0])", [ref]$lo)) { return $false }
      if (-not [double]::TryParse("$($arr[1])", [ref]$hi)) { return $false }
      return ($m -ge $lo -and $m -le $hi)
    }
    '==' { return ("$Measured" -eq "$Expected") }
    '!=' { return ("$Measured" -ne "$Expected") }
    default { return $false }
  }
}

function Test-ThresholdValueEqual {
  # Structural equality between a policy threshold 'value' and the results copy's
  # 'value' (scalar OR the [lo,hi] array used by 'between').
  [OutputType([bool])]
  param($A, $B)
  $aa = @($A); $bb = @($B)
  if ($aa.Count -ne $bb.Count) { return $false }
  for ($i = 0; $i -lt $aa.Count; $i++) { if ([string]$aa[$i] -ne [string]$bb[$i]) { return $false } }
  return $true
}

function Test-HardwareEvidence {
  # Validate a hardware-results.json + its evidence for one gate. Returns
  # { Ok; Errors; Checks }. Thresholds/required evidence + all item/threshold/
  # evidence SHAPE are read from the AUTHORITATIVE policy (not the operator-editable
  # results copy); only 'measured', 'status', 'operator', and the evidence
  # 'size'/'sha256' are operator-supplied. Evidence is cryptographically bound by
  # re-hashing each file. Never auto-promotes anything.
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
    [string]$HardwarePolicySha256 = '',
    [string]$ReparseBase = '',
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
  function Test-SetsEqual($a, $b) {
    $sa = @(@($a) | Sort-Object); $sb = @(@($b) | Sort-Object)
    if ($sa.Count -ne $sb.Count) { return $false }
    for ($i = 0; $i -lt $sa.Count; $i++) { if ([string]$sa[$i] -ne [string]$sb[$i]) { return $false } }
    return $true
  }

  if (-not (Test-Path $ResultsPath)) {
    Add-Check 'results-exists' $false "missing $ResultsPath"
    return [pscustomobject]@{ Ok = $false; Errors = $errors; Checks = $checks }
  }
  $results = Read-JsonFile $ResultsPath
  $evDirFull = Get-FullPathSafe $EvidenceDir

  # -- Reparse-point defence (fail-closed) ------------------------------------
  # Reject a reparse evidence root, any reparse file/dir under it, or a reparse
  # ANCESTOR directory between $ReparseBase (default: the evidence dir) and the
  # evidence dir. Lexical containment alone is not enough on Windows.
  $reBase = $ReparseBase; if (-not $reBase) { $reBase = $evDirFull }
  $reErr = $null; $reAnc = $null; $reUnder = @()
  try {
    $reAnc = Find-ReparsePointInChain -Base $reBase -Full $evDirFull
    if (Test-Path -LiteralPath $evDirFull) { $reUnder = @(Get-ContainedReparsePoints $evDirFull) }
  } catch { $reErr = $_.Exception.Message }
  if ($reErr) { Add-Check 'evidence-no-reparse' $false "reparse-point inspection failed (fail-closed): $reErr" }
  else { Add-Check 'evidence-no-reparse' (($null -eq $reAnc) -and ($reUnder.Count -eq 0)) ("evidence root/subtree/ancestors must not be or contain a symlink/junction/reparse point: " + [string]::Join('; ', @(@($reAnc) + $reUnder | Where-Object { $_ } | Select-Object -First 3))) }

  # -- authoritative policy schema / version (loaded from tracked git) --------
  Add-Check 'policy-schema' (([string](Get-PsObjectProperty $Policy 'schema')) -eq 'plane-radar/hardware-acceptance-policy') "hardware policy schema must be plane-radar/hardware-acceptance-policy"
  Add-Check 'policy-schema-version' ([int](Get-PsObjectProperty $Policy 'schema_version') -eq 1) "hardware policy schema_version must be 1"

  # -- schema / schema_version / policy_ref / policy_sha256 -------------------
  Add-Check 'results-schema' (([string](Get-PsObjectProperty $results 'schema')) -eq 'plane-radar/hardware-results') "unexpected results schema"
  Add-Check 'results-schema-version' ([int](Get-PsObjectProperty $results 'schema_version') -eq 1) "results schema_version must be 1"
  Add-Check 'results-policy-ref' (([string](Get-PsObjectProperty $results 'policy_ref')) -eq 'scripts/hardware-acceptance-policy.json') "results policy_ref must be scripts/hardware-acceptance-policy.json"
  $resPolSha = [string](Get-PsObjectProperty $results 'policy_sha256')
  if ($HardwarePolicySha256) {
    Add-Check 'results-policy-sha' ((Test-Sha256Hex $resPolSha) -and ($resPolSha -eq $HardwarePolicySha256)) "results.policy_sha256 must equal the SHA-256 of the tracked git-blob hardware-acceptance-policy.json used ($HardwarePolicySha256)"
  } else {
    Add-Check 'results-policy-sha' (Test-Sha256Hex $resPolSha) "results.policy_sha256 must be a 64-hex SHA-256"
  }

  # -- top-level binding to the exact flashed default image -------------------
  $b = Get-PsObjectProperty $results 'binding'
  Add-Check 'binding-commit' (([string](Get-PsObjectProperty $b 'commit')) -eq $ExpectedCommit) "results must bind to release commit $ExpectedCommit"
  Add-Check 'binding-environment' (([string](Get-PsObjectProperty $b 'environment')) -eq $ExpectedEnv) "default environment must be $ExpectedEnv"
  Add-Check 'binding-default-image' (([string](Get-PsObjectProperty $b 'default_image')) -eq $ExpectedDefaultRel) "default image path must be $ExpectedDefaultRel"
  $expDefSha = ''; if ($ImageSha.ContainsKey($ExpectedDefaultRel)) { $expDefSha = $ImageSha[$ExpectedDefaultRel] }
  $expDefSize = -1; if ($ImageSize.ContainsKey($ExpectedDefaultRel)) { $expDefSize = $ImageSize[$ExpectedDefaultRel] }
  Add-Check 'binding-image-sha' (([string](Get-PsObjectProperty $b 'image_sha256')) -eq $expDefSha -and $expDefSha.Length -eq 64) "binding image_sha256 must equal the flashed default image SHA-256"
  Add-Check 'binding-image-size' ([int64](Get-PsObjectProperty $b 'image_size') -eq [int64]$expDefSize) "binding image_size must equal the flashed default image byte length"

  # -- exact, duplicate-free item set across ALL policy items -----------------
  $resItems = @(Get-PsObjectProperty $results 'items')
  $resIds = @($resItems | ForEach-Object { [string](Get-PsObjectProperty $_ 'id') })
  $policyIds = @($Policy.items | ForEach-Object { [string]$_.id })
  $resDupFree = ($resIds.Count -eq (@($resIds | Select-Object -Unique).Count))
  Add-Check 'items-exact-set' ($resDupFree -and (Test-SetsEqual $policyIds $resIds)) "results must contain EXACTLY the policy item ids (no missing/extra/duplicate)"

  $resById = @{}
  foreach ($ri in $resItems) { $resById[[string](Get-PsObjectProperty $ri 'id')] = $ri }

  $selected = @($Policy.items | Where-Object { $Gate -eq 'all' -or $_.gate -eq $Gate })
  if ($selected.Count -eq 0) { Add-Check 'gate-has-items' $false "no policy items for gate '$Gate'" }

  foreach ($pi in $selected) {
    $id = [string]$pi.id
    if (-not $resById.ContainsKey($id)) { Add-Check "item/$id" $false "item missing from results"; continue }
    $ri = $resById[$id]
    $informational = $false
    $infoProp = $pi.PSObject.Properties['informational']
    if ($infoProp -and [bool]$infoProp.Value) { $informational = $true }

    # per-item binding: exact gate/order/environment/image + flashed image size+SHA
    $imgRel = [string]$pi.image
    $expSha = ''; if ($ImageSha.ContainsKey($imgRel)) { $expSha = $ImageSha[$imgRel] }
    $expSize = -1; if ($ImageSize.ContainsKey($imgRel)) { $expSize = $ImageSize[$imgRel] }
    $bindOk = ((([string](Get-PsObjectProperty $ri 'gate')) -eq [string]$pi.gate) -and `
               ([int](Get-PsObjectProperty $ri 'order') -eq [int]$pi.order) -and `
               (([string](Get-PsObjectProperty $ri 'environment')) -eq [string]$pi.environment) -and `
               (([string](Get-PsObjectProperty $ri 'image')) -eq $imgRel) -and `
               ([int64](Get-PsObjectProperty $ri 'image_size') -eq [int64]$expSize) -and `
               (([string](Get-PsObjectProperty $ri 'image_sha256')) -eq $expSha) -and ($expSha.Length -eq 64))
    Add-Check "item-binding/$id" $bindOk "item must bind exact policy gate/order/environment/image and the flashed image size + SHA-256"

    # thresholds: EXACT key set + op/value/unit (operator may only edit 'measured')
    $resThr = @(Get-PsObjectProperty $ri 'thresholds')
    $resThrKeys = @($resThr | ForEach-Object { [string](Get-PsObjectProperty $_ 'key') })
    $polThrKeys = @($pi.thresholds | ForEach-Object { [string]$_.key })
    $thrDupFree = ($resThrKeys.Count -eq (@($resThrKeys | Select-Object -Unique).Count))
    $thrSetOk = ($thrDupFree -and (Test-SetsEqual $polThrKeys $resThrKeys))
    Add-Check "thresholds-set/$id" $thrSetOk "results thresholds must be EXACTLY the policy keys (no missing/extra/duplicate)"
    $resThrByKey = @{}; foreach ($rt in $resThr) { $resThrByKey[[string](Get-PsObjectProperty $rt 'key')] = $rt }
    $thrMetaOk = $thrSetOk
    foreach ($pt in $pi.thresholds) {
      $k = [string]$pt.key
      if (-not $resThrByKey.ContainsKey($k)) { $thrMetaOk = $false; continue }
      $rt = $resThrByKey[$k]
      if (([string](Get-PsObjectProperty $rt 'op')) -ne [string]$pt.op) { $thrMetaOk = $false }
      if (([string](Get-PsObjectProperty $rt 'unit')) -ne [string]$pt.unit) { $thrMetaOk = $false }
      if (-not (Test-ThresholdValueEqual (Get-PsObjectProperty $rt 'value') $pt.value)) { $thrMetaOk = $false }
    }
    Add-Check "thresholds-meta/$id" $thrMetaOk "results threshold op/value/unit must equal policy (operator may only edit 'measured')"

    # evidence: EXACT name set + all names safe
    $resEv = @(Get-PsObjectProperty $ri 'evidence')
    $resEvNames = @($resEv | ForEach-Object { [string](Get-PsObjectProperty $_ 'name') })
    $polEvNames = @($pi.evidence | ForEach-Object { [string]$_.name })
    $evDupFree = ($resEvNames.Count -eq (@($resEvNames | Select-Object -Unique).Count))
    Add-Check "evidence-set/$id" ($evDupFree -and (Test-SetsEqual $polEvNames $resEvNames)) "results evidence must be EXACTLY the policy evidence names (no missing/extra/duplicate)"
    $evSafeOk = $true
    foreach ($rn in $resEvNames) { if (-not (Test-RelPathSafe $rn)) { $evSafeOk = $false } }
    Add-Check "evidence-safe/$id" $evSafeOk "every evidence filename must be a safe relative path"

    if ($informational) { continue }  # recorded but not gating

    # status must be an explicit pass
    $status = [string](Get-PsObjectProperty $ri 'status')
    Add-Check "status/$id" ($status -eq 'pass') "status must be 'pass' (got '$status'; pending/blocked/waived/fail are NOT pass)"

    # mandatory pass items require a non-empty operator name + REAL ISO date
    if ([bool]$pi.mandatory) {
      $op = Get-PsObjectProperty $ri 'operator'
      $opName = [string](Get-PsObjectProperty $op 'name')
      $opDate = [string](Get-PsObjectProperty $op 'date')
      $dateOk = Test-IsoCalendarDate $opDate
      Add-Check "operator/$id" (($opName.Trim().Length -gt 0) -and $dateOk) "mandatory pass item requires a non-empty operator name and a REAL ISO calendar date (YYYY-MM-DD; e.g. 2026-99-99 is rejected)"
    }

    # evidence files: exist + non-empty + recorded size/lowercase SHA-256 match
    $resEvByName = @{}; foreach ($re in $resEv) { $resEvByName[[string](Get-PsObjectProperty $re 'name')] = $re }
    foreach ($pe in $pi.evidence) {
      $name = [string]$pe.name
      $safe = Test-RelPathSafe $name
      $file = Join-Path $evDirFull ($name -replace '/', '\')
      $inside = $safe -and (Test-PathInside -Base $evDirFull -Candidate $file)
      $exists = $inside -and (Test-Path $file -PathType Leaf)
      $nonEmpty = $exists -and ((Get-Item $file).Length -gt 0)
      $hashOk = $false
      if ($nonEmpty -and $resEvByName.ContainsKey($name)) {
        $re = $resEvByName[$name]
        $recSize = Get-PsObjectProperty $re 'size'
        $recSha = [string](Get-PsObjectProperty $re 'sha256')
        $actualSize = (Get-Item $file).Length
        $actualSha = Get-Sha256Hex $file
        $hashOk = (($null -ne $recSize) -and ([int64]$recSize -eq [int64]$actualSize) -and (Test-Sha256Hex $recSha) -and ($recSha -eq $actualSha))
      }
      Add-Check "evidence/$id/$name" ($safe -and $inside -and $exists -and $nonEmpty -and $hashOk) "required evidence must exist, be non-empty, and its recorded size + lowercase SHA-256 must match the file"
    }

    # measured values must satisfy the AUTHORITATIVE policy thresholds
    $measuredByKey = @{}
    foreach ($rt in $resThr) { $measuredByKey[[string](Get-PsObjectProperty $rt 'key')] = (Get-PsObjectProperty $rt 'measured') }
    foreach ($pt in $pi.thresholds) {
      $key = [string]$pt.key
      $measured = $null; if ($measuredByKey.ContainsKey($key)) { $measured = $measuredByKey[$key] }
      $ok = Test-Threshold -Op ([string]$pt.op) -Measured $measured -Expected $pt.value
      Add-Check "threshold/$id/$key" $ok "measured '$measured' must be $($pt.op) $($pt.value) $($pt.unit)"
    }
  }

  return [pscustomobject]@{ Ok = ($errors.Count -eq 0); Errors = $errors; Checks = $checks }
}
