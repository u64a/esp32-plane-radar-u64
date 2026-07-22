<#
.SYNOPSIS
  Source-level reproducible-build policy gate (Phase 12 final).

.DESCRIPTION
  Fails closed unless the tracked build configuration still enforces
  byte-reproducible-across-worktree-paths firmware builds. This is the SOURCE
  half of the defense; the BUILT-OUTPUT half is the per-environment
  'path-independence' + 'image-sha'(firmware.elf) + 'elf-binding' proof
  invariants that build-release/verify-release re-derive from the actual
  firmware.elf/firmware.bin (see scripts/release-common.ps1). Together they are
  not a "flag-only" check: this gate guarantees the DWARF-stripping flags stay
  in platformio.ini, and the proof invariants guarantee the shipped ELF really
  contains no worktree path and is bound to the app image.

  Invariants asserted here:
    1. [env:supermini] declares build_unflags containing -ggdb AND build_flags
       containing -g0 (strip project DWARF so no absolute build-worktree path is
       embedded in firmware.elf debug metadata).
    2. Every firmware env (supermini + the four supermini-* envs) extends
       supermini and re-includes ${env:supermini.build_flags}, so the -ggdb
       unflag and -g0 flag propagate to all five release envs.
    3. The native / native-diag / native-gfx test envs do NOT strip debug info
       (they are separate envs and must keep their symbols/DWARF for tests).
    4. The tracked release-policy.json carries the reproducibility + app_descriptor
       sections and an exact firmware_elf SHA-256 anchor + size for every env, so
       the ELF proof input is Git-policy anchored and its embedded-SHA binding
       offset has tracked provenance.

  Runs identically under Windows PowerShell 5.1 and pwsh 7. Throws on any
  violation (fail-closed); prints per-check status on success.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
[Environment]::CurrentDirectory = $repoRoot

$firmwareEnvs = @('supermini', 'supermini-worker', 'supermini-quiet', 'supermini-diag', 'supermini-worker-diag')
$nativeEnvs = @('native', 'native-diag', 'native-gfx')

function Get-IniSections {
  # Parse a platformio.ini into an ordered map of section-name -> raw body text
  # (everything up to the next '[' header). Comments/blank lines are retained;
  # callers match tokens with regex.
  param([Parameter(Mandatory)][string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { throw "platformio.ini not found: $Path" }
  $lines = Get-Content -LiteralPath $Path
  $sections = [ordered]@{}
  $current = $null
  $sb = $null
  foreach ($line in $lines) {
    $m = [regex]::Match($line, '^\s*\[(?<name>[^\]]+)\]\s*$')
    if ($m.Success) {
      if ($null -ne $current) { $sections[$current] = $sb.ToString() }
      $current = $m.Groups['name'].Value
      $sb = New-Object System.Text.StringBuilder
      continue
    }
    if ($null -ne $sb) { [void]$sb.AppendLine($line) }
  }
  if ($null -ne $current) { $sections[$current] = $sb.ToString() }
  return $sections
}

$fail = New-Object System.Collections.Generic.List[string]
function Assert-True([string]$label, [bool]$cond) {
  if ($cond) { Write-Host "  [ok] $label" }
  else { Write-Host "  [FAIL] $label" -ForegroundColor Red; $fail.Add($label) }
}

Write-Host "=== reproducible-build source policy gate ===" -ForegroundColor Cyan
$iniPath = Join-Path $repoRoot 'platformio.ini'
$sections = Get-IniSections -Path $iniPath

# -- 1. Base [env:supermini] strips project DWARF ---------------------------
$base = ''
if ($sections.Contains('env:supermini')) { $base = [string]$sections['env:supermini'] }
Assert-True "[env:supermini] present" ($base.Length -gt 0)
# build_unflags must remove -ggdb (so the framework's -ggdb no longer applies),
# and build_flags must add -g0.
Assert-True "[env:supermini] build_unflags removes -ggdb" ($base -match '(?m)^\s*build_unflags\b' -and $base -match '-ggdb')
Assert-True "[env:supermini] build_flags add -g0" ($base -match '(?m)(^|\s)-g0(\s|$)')

# -- 2. Every firmware env inherits the DWARF-strip policy -------------------
# Children extend a supermini-family env (supermini-worker-diag extends the
# supermini-worker link of the chain) and re-include that env's build_flags, so
# build_unflags=-ggdb and -g0 propagate transitively to all five release envs.
foreach ($e in $firmwareEnvs) {
  if ($e -eq 'supermini') { continue }
  $body = ''
  if ($sections.Contains("env:$e")) { $body = [string]$sections["env:$e"] }
  Assert-True "[env:$e] present" ($body.Length -gt 0)
  Assert-True "[env:$e] extends a supermini-family env" ($body -match '(?m)^\s*extends\s*=\s*env:supermini(-[\w-]+)?\s*$')
  Assert-True "[env:$e] re-includes a supermini-family build_flags" ($body -match '\$\{env:supermini(-[\w-]+)?\.build_flags\}')
}

# -- 3. Native/test envs are NOT DWARF-stripped -----------------------------
foreach ($e in $nativeEnvs) {
  if (-not $sections.Contains("env:$e")) { continue }
  $body = [string]$sections["env:$e"]
  $stripped = ($body -match '(?m)^\s*build_unflags\b[\s\S]*-ggdb') -or ($body -match '(?m)(^|\s)-g0(\s|$)')
  Assert-True "[env:$e] keeps debug info (no -g0 / -ggdb unflag)" (-not $stripped)
}

# -- 4. Tracked policy carries the ELF anchors + provenance -----------------
. "$PSScriptRoot\release-common.ps1"
$git = Get-RepoGitState $repoRoot
if (-not (Test-GitCommitPresent -RepoRoot $repoRoot -Commit $git.Commit)) {
  throw "HEAD commit $($git.Commit) is not present as a git object; cannot load tracked policy."
}
$gp = Get-GitPolicy -RepoRoot $repoRoot -Commit $git.Commit -RelPath 'scripts/release-policy.json'
$policy = $gp.Object

$repro = Get-PsObjectProperty $policy 'reproducibility'
Assert-True "policy has reproducibility section" ($null -ne $repro)
if ($repro) {
  Assert-True "policy reproducibility.debug_policy = strip-dwarf-from-project" (([string](Get-PsObjectProperty $repro 'debug_policy')) -eq 'strip-dwarf-from-project')
  Assert-True "policy reproducibility.forbidden_elf_path_token present" (([string](Get-PsObjectProperty $repro 'forbidden_elf_path_token')).Length -gt 0)
}

$ad = Get-PsObjectProperty $policy 'app_descriptor'
Assert-True "policy has app_descriptor section" ($null -ne $ad)
if ($ad) {
  $adOff = Get-PsObjectProperty $ad 'elf_sha256_offset'
  $adLen = Get-PsObjectProperty $ad 'elf_sha256_length'
  Assert-True "policy app_descriptor.elf_sha256_offset is a non-negative int" (($null -ne $adOff) -and ([int]$adOff -ge 0))
  Assert-True "policy app_descriptor.elf_sha256_length = 32" (($null -ne $adLen) -and ([int]$adLen -eq 32))
}

foreach ($ep in $policy.environments) {
  $sha = Get-PsObjectProperty $ep 'sha256'
  $elfSha = ''; if ($sha) { $elfSha = [string](Get-PsObjectProperty $sha 'firmware_elf') }
  Assert-True "[$($ep.name)] policy has exact firmware_elf SHA-256 anchor" (Test-Sha256Hex $elfSha)
  $res = Get-PsObjectProperty $ep 'resources'
  $elfSize = Get-PsObjectProperty $res 'firmware_elf'
  Assert-True "[$($ep.name)] policy has firmware_elf size" (($null -ne $elfSize) -and ([int64]$elfSize -gt 0))
}

if ($fail.Count -gt 0) {
  throw "reproducible-build source policy gate FAILED ($($fail.Count)):`n  " + [string]::Join("`n  ", $fail)
}
Write-Host "reproducible-build source policy gate PASSED." -ForegroundColor Green
