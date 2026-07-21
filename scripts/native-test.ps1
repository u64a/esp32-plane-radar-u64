[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
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
