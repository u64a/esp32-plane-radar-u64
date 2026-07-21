[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot

# scripts/native-test.ps1 is ALWAYS read-only with respect to the checked-in
# render goldens (test/golden): it is a certification gate and NEVER an update
# path. If the parent environment opts into golden rewriting
# (PLANE_RADAR_UPDATE_GOLDENS=1), a passing native-gfx gate could silently
# overwrite checked-in expectations, so fail closed HERE, before any build or
# test runs (a narrow, prelude-free negative check). Intentional golden updates
# are DIRECT ONLY: run `pio test -e native-gfx` with PLANE_RADAR_UPDATE_GOLDENS=1
# yourself; this script will never do it for you.
if ($env:PLANE_RADAR_UPDATE_GOLDENS -eq "1") {
  $msg = "scripts/native-test.ps1 is read-only and refuses to run with " +
    "PLANE_RADAR_UPDATE_GOLDENS=1: this certified gate never rewrites render " +
    "goldens. To intentionally update goldens, run a direct " +
    "'pio test -e native-gfx' invocation with PLANE_RADAR_UPDATE_GOLDENS=1."
  throw $msg
}

# Phase 11: run the airport-data offline gate fail-fast before any PlatformIO tests.
Write-Host "--- verify-airport-data gate ---"
$airportGate = Join-Path $PSScriptRoot "verify-airport-data.ps1"
# The gate runs under $ErrorActionPreference = "Stop" and THROWS on any violation,
# which propagates and aborts this script. Do not inspect $LASTEXITCODE afterward:
# a successful PowerShell script does not reset a previous native exit code, so a
# stale value would be unsafe. Rely on the thrown exception instead.
& $airportGate -ProjectRoot $projectRoot
Write-Host ""
Write-Host "--- verify-egress-policy gate ---"
$egressGate = Join-Path $PSScriptRoot "verify-egress-policy.ps1"
& $egressGate -ProjectRoot $projectRoot
Write-Host ""

$toolchainVersion = "2.8.0"
$gccVersion = "16.1.0"
$assetSha256 =
  "6252bf34fe2231a55ac7f03d482b36d2c7c58697990551bba508102cfb3f342e"
$cacheRoot = Join-Path $env:LOCALAPPDATA `
  "esp32-plane-radar\native-toolchains\w64devkit-$toolchainVersion"
$toolchainBin = Join-Path $cacheRoot "bin"
$cCompiler = Join-Path $toolchainBin "gcc.exe"
$compiler = Join-Path $toolchainBin "g++.exe"
$markerPath = Join-Path $cacheRoot "plane-radar-toolchain.json"
$pioCommand = Get-Command pio -ErrorAction SilentlyContinue
$pio = if ($pioCommand) {
  $pioCommand.Source
} else {
  Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
}

if (-not (Test-Path $pio)) {
  throw "PlatformIO Core not found. Install requirements-dev.txt first."
}

$version = & $pio --version
if ($LASTEXITCODE -ne 0 -or $version -ne "PlatformIO Core, version 6.1.19") {
  throw "PlatformIO Core 6.1.19 is required; found: $version"
}

if (-not (Test-Path $cCompiler) -or
    -not (Test-Path $compiler) -or
    -not (Test-Path $markerPath)) {
  throw "Pinned native compiler not found. Run .\scripts\setup-native-toolchain.ps1 first."
}

$marker = Get-Content -Raw $markerPath | ConvertFrom-Json
if ($marker.toolchain_version -ne $toolchainVersion -or
    $marker.gcc_version -ne $gccVersion -or
    $marker.asset_sha256 -ne $assetSha256) {
  throw "Cached native compiler metadata does not match the repository pin."
}

$installedGccVersion = (& $compiler -dumpfullversion | Select-Object -First 1).Trim()
if ($LASTEXITCODE -ne 0 -or $installedGccVersion -ne $gccVersion) {
  throw "GCC $gccVersion is required; found: $installedGccVersion"
}

$testInfo = New-Object System.Diagnostics.ProcessStartInfo
$testInfo.FileName = $pio
$testInfo.WorkingDirectory = $projectRoot
$testInfo.Arguments = "test -e native"
$testInfo.UseShellExecute = $false
$testInfo.EnvironmentVariables["PATH"] =
  "$toolchainBin$([System.IO.Path]::PathSeparator)$($testInfo.EnvironmentVariables["PATH"])"
$testInfo.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8"

$testProcess = [System.Diagnostics.Process]::Start($testInfo)
$testProcess.WaitForExit()
if ($testProcess.ExitCode -ne 0) {
  throw "PlatformIO native tests failed with exit code $($testProcess.ExitCode)"
}

# Phase 10: also run the diagnostics-on native suite ([env:native-diag],
# PLANE_RADAR_DIAGNOSTICS=1) which verifies the conditional WorkerResult field.
$diagInfo = New-Object System.Diagnostics.ProcessStartInfo
$diagInfo.FileName = $pio
$diagInfo.WorkingDirectory = $projectRoot
$diagInfo.Arguments = "test -e native-diag"
$diagInfo.UseShellExecute = $false
$diagInfo.EnvironmentVariables["PATH"] =
  "$toolchainBin$([System.IO.Path]::PathSeparator)$($diagInfo.EnvironmentVariables["PATH"])"
$diagInfo.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8"

$diagProcess = [System.Diagnostics.Process]::Start($diagInfo)
$diagProcess.WaitForExit()
if ($diagProcess.ExitCode -ne 0) {
  throw "PlatformIO native-diag tests failed with exit code $($diagProcess.ExitCode)"
}

# Phase 12: headless LovyanGFX render + golden-image gate ([env:native-gfx]).
# Runs AFTER the Phase 11 pure-logic native/native-diag gates and uses the same
# pinned w64devkit compiler PATH. It compiles the real production UI drawing code
# into a native binary, renders every named scene into an in-RAM 240x240 RGB565
# LovyanGFX sprite canvas, and byte-compares the captured framebuffer against the
# checked-in golden BMPs (test/golden). Fully offline/headless: no SDL2, no
# window, no hardware. ALWAYS read-only: this gate never writes goldens. The
# script fails closed at the top on PLANE_RADAR_UPDATE_GOLDENS=1, and as defence
# in depth the key is stripped from the child below, so the native-gfx binary can
# never observe an inherited update opt-in. Intentional golden updates are
# direct-only (`pio test -e native-gfx` with PLANE_RADAR_UPDATE_GOLDENS=1). Fail
# fast.
$gfxInfo = New-Object System.Diagnostics.ProcessStartInfo
$gfxInfo.FileName = $pio
$gfxInfo.WorkingDirectory = $projectRoot
$gfxInfo.Arguments = "test -e native-gfx"
$gfxInfo.UseShellExecute = $false
$gfxInfo.EnvironmentVariables["PATH"] =
  "$toolchainBin$([System.IO.Path]::PathSeparator)$($gfxInfo.EnvironmentVariables["PATH"])"
$gfxInfo.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8"
# Defence in depth: even though this script already failed closed above when
# PLANE_RADAR_UPDATE_GOLDENS=1, defensively clear the key from the child process
# environment so the native-gfx gate can NEVER inherit an update opt-in.
if ($gfxInfo.EnvironmentVariables.ContainsKey("PLANE_RADAR_UPDATE_GOLDENS")) {
  [void]$gfxInfo.EnvironmentVariables.Remove("PLANE_RADAR_UPDATE_GOLDENS")
}

$gfxProcess = [System.Diagnostics.Process]::Start($gfxInfo)
$gfxProcess.WaitForExit()
if ($gfxProcess.ExitCode -ne 0) {
  throw "PlatformIO native-gfx render golden tests failed with exit code $($gfxProcess.ExitCode)"
}
