[CmdletBinding()]
param()

# Proves that the SnapshotStoreTestAccess friend seam cannot leak into a
# firmware build:
#   1. Source check: platformio.ini defines PLANE_RADAR_NATIVE_TEST_ACCESS only
#      under [env:native] build_flags, never under [env:supermini].
#   2. Compile check: test/support/snapshot_store_test_access.h fails to
#      compile without the macro (the guard in the header itself), and
#      compiles cleanly with it -- using the same pinned native compiler as
#      scripts/native-test.ps1, with no ESP32/Arduino toolchain involved.

$ErrorActionPreference = "Stop"

# Invoke a native executable deterministically across Windows PowerShell 5.1 and
# pwsh 7. This exists because two shell-specific hazards otherwise break the gate:
#   * Under $ErrorActionPreference='Stop', Windows PowerShell 5.1 turns any native
#     stderr into a terminating RemoteException, so an intentionally-failing
#     compile aborts before we can inspect its exit code. We locally relax the
#     preference to 'Continue' (restored in finally) so stderr stays non-fatal.
#   * Piping a native command into Select-Object (or any pipeline that stops
#     early) can leave $LASTEXITCODE null in pwsh 7 because the native process is
#     torn down before it reports. We capture the exit code immediately after the
#     call, on the same statement, and never read the global $LASTEXITCODE later.
# Arguments are passed through @ArgumentList splatting so path arguments with
# spaces are forwarded verbatim. stderr is discarded; callers assert on ExitCode.
function Invoke-NativeGuarded {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory)][string]$FilePath,
    [string[]]$ArgumentList = @()
  )
  $previousErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try {
    $stdout = & $FilePath @ArgumentList 2>$null
    $exitCode = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousErrorActionPreference
  }
  if ($null -eq $exitCode) {
    throw "Native command '$FilePath' did not report an exit code."
  }
  return [pscustomobject]@{
    ExitCode = $exitCode
    StdOut   = $stdout
  }
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$toolchainVersion = "2.8.0"
$gccVersion = "16.1.0"
$cacheRoot = Join-Path $env:LOCALAPPDATA `
  "esp32-plane-radar\native-toolchains\w64devkit-$toolchainVersion"
$compiler = Join-Path $cacheRoot "bin\g++.exe"

if (-not (Test-Path $compiler)) {
  throw "Pinned native compiler not found. Run .\scripts\setup-native-toolchain.ps1 first."
}

$dumpResult = Invoke-NativeGuarded -FilePath $compiler -ArgumentList @("-dumpfullversion")
$installedGccVersion = ([string]($dumpResult.StdOut | Select-Object -First 1)).Trim()
if ($dumpResult.ExitCode -ne 0 -or $installedGccVersion -ne $gccVersion) {
  throw "GCC $gccVersion is required; found: $installedGccVersion"
}

# --- 1. Source check on platformio.ini ------------------------------------

$iniPath = Join-Path $projectRoot "platformio.ini"
$iniText = Get-Content -Raw $iniPath

function Get-EnvSection([string]$text, [string]$sectionName) {
  $pattern = "(?ms)^\[$([regex]::Escape($sectionName))\]\r?\n(.*?)(?=\r?\n\[|\z)"
  $match = [regex]::Match($text, $pattern)
  if (-not $match.Success) {
    throw "Could not find [$sectionName] section in $iniPath"
  }
  return $match.Groups[1].Value
}

$nativeSection = Get-EnvSection $iniText "env:native"
$supermini = Get-EnvSection $iniText "env:supermini"

if ($nativeSection -notmatch [regex]::Escape("-DPLANE_RADAR_NATIVE_TEST_ACCESS=1")) {
  throw "[env:native] must define -DPLANE_RADAR_NATIVE_TEST_ACCESS=1 in build_flags"
}
if ($supermini -match "PLANE_RADAR_NATIVE_TEST_ACCESS") {
  throw "[env:supermini] must never reference PLANE_RADAR_NATIVE_TEST_ACCESS"
}

Write-Host "OK: PLANE_RADAR_NATIVE_TEST_ACCESS is defined only in [env:native]."

# --- 2. Compile check -------------------------------------------------------

$includeRoot = Join-Path $projectRoot "include"
$testAccessHeader = Join-Path $projectRoot "test\support\snapshot_store_test_access.h"
$probeSource = Join-Path $projectRoot "scripts\.native-test-access-gate-probe.cpp"

Set-Content -Encoding utf8 $probeSource @"
#include "snapshot_store_test_access.h"
int main() { return 0; }
"@

try {
  # Firmware-equivalent compile: no PLANE_RADAR_NATIVE_TEST_ACCESS defined, same
  # as every flag set the supermini env uses. This must fail via the header's
  # own #error guard, proving the accessor cannot compile without the macro.
  $firmwareArgs = @(
    "-std=gnu++17", "-fsyntax-only",
    "-I", $includeRoot,
    "-I", (Split-Path $testAccessHeader -Parent),
    $probeSource
  )
  $firmwareResult = Invoke-NativeGuarded -FilePath $compiler -ArgumentList $firmwareArgs
  if ($firmwareResult.ExitCode -eq 0) {
    throw "snapshot_store_test_access.h compiled WITHOUT PLANE_RADAR_NATIVE_TEST_ACCESS; the firmware gate is broken."
  }
  Write-Host "OK: snapshot_store_test_access.h fails to compile without PLANE_RADAR_NATIVE_TEST_ACCESS (firmware-equivalent build)."

  # Native-test-equivalent compile: macro defined, must succeed.
  $nativeArgs = @(
    "-std=gnu++17", "-fsyntax-only",
    "-DPLANE_RADAR_NATIVE_TEST_ACCESS=1",
    "-I", $includeRoot,
    "-I", (Split-Path $testAccessHeader -Parent),
    $probeSource
  )
  $nativeResult = Invoke-NativeGuarded -FilePath $compiler -ArgumentList $nativeArgs
  if ($nativeResult.ExitCode -ne 0) {
    throw "snapshot_store_test_access.h failed to compile WITH PLANE_RADAR_NATIVE_TEST_ACCESS defined."
  }
  Write-Host "OK: snapshot_store_test_access.h compiles with PLANE_RADAR_NATIVE_TEST_ACCESS=1 (native-test build)."
} finally {
  Remove-Item -Force $probeSource -ErrorAction SilentlyContinue
}

Write-Host "Native test-access gate verified: firmware builds cannot see SnapshotStoreTestAccess."
