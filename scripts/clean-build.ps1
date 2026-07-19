[CmdletBinding()]
param(
  [string]$Environment = "supermini"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
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

$pioDirectory = Join-Path $projectRoot ".pio"
if (Test-Path $pioDirectory) {
  Remove-Item -Recurse -Force $pioDirectory
}
if (Test-Path $pioDirectory) {
  throw "Failed to remove $pioDirectory"
}

Push-Location $projectRoot
try {
  & $pio run -e $Environment
  if ($LASTEXITCODE -ne 0) {
    throw "PlatformIO build failed with exit code $LASTEXITCODE"
  }
} finally {
  Pop-Location
}
