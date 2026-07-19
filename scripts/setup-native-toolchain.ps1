[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$toolchainVersion = "2.8.0"
$gccVersion = "16.1.0"
$assetName = "w64devkit-x64-2.8.0.7z.exe"
$assetUrl =
  "https://github.com/skeeto/w64devkit/releases/download/v2.8.0/$assetName"
$assetSha256 =
  "6252bf34fe2231a55ac7f03d482b36d2c7c58697990551bba508102cfb3f342e"

if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
  throw "LOCALAPPDATA is required for the project-local native toolchain cache."
}

$cacheBase = Join-Path $env:LOCALAPPDATA "esp32-plane-radar\native-toolchains"
$cacheRoot = Join-Path $cacheBase "w64devkit-$toolchainVersion"
$cCompiler = Join-Path $cacheRoot "bin\gcc.exe"
$compiler = Join-Path $cacheRoot "bin\g++.exe"
$markerPath = Join-Path $cacheRoot "plane-radar-toolchain.json"

function Get-CompilerVersion([string]$compilerPath) {
  $versionOutput = & $compilerPath -dumpfullversion
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to query compiler version from $compilerPath"
  }
  return ($versionOutput | Select-Object -First 1).Trim()
}

if (Test-Path $cacheRoot) {
  if (-not (Test-Path $cCompiler) -or
      -not (Test-Path $compiler) -or
      -not (Test-Path $markerPath)) {
    throw "Incomplete native toolchain cache at $cacheRoot. Remove it and rerun this script."
  }

  $marker = Get-Content -Raw $markerPath | ConvertFrom-Json
  if ($marker.asset_url -ne $assetUrl -or
      $marker.asset_sha256 -ne $assetSha256 -or
      $marker.toolchain_version -ne $toolchainVersion -or
      $marker.gcc_version -ne $gccVersion) {
    throw "Native toolchain cache metadata does not match the repository pin: $markerPath"
  }

  $cachedVersion = Get-CompilerVersion $compiler
  if ($cachedVersion -ne $gccVersion) {
    throw "Expected GCC $gccVersion in cache; found $cachedVersion"
  }

  Write-Host "w64devkit $toolchainVersion with GCC $gccVersion is ready at $cacheRoot"
  return
}

New-Item -ItemType Directory -Force $cacheBase | Out-Null
$downloadPath = Join-Path $cacheBase "$assetName.$PID.download"
$stagingRoot = Join-Path $cacheBase "extract-$toolchainVersion-$PID"

try {
  Write-Host "Downloading pinned w64devkit $toolchainVersion asset..."
  Invoke-WebRequest -Uri $assetUrl -OutFile $downloadPath

  $actualSha256 = (Get-FileHash -Algorithm SHA256 $downloadPath).Hash.ToLowerInvariant()
  if ($actualSha256 -ne $assetSha256) {
    throw "SHA-256 mismatch for $assetName. Expected $assetSha256; found $actualSha256"
  }

  $signature = Get-AuthenticodeSignature $downloadPath
  if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
      $null -eq $signature.SignerCertificate) {
    throw "Authenticode validation failed for ${assetName}: $($signature.Status) $($signature.StatusMessage)"
  }

  New-Item -ItemType Directory $stagingRoot | Out-Null
  $extractInfo = New-Object System.Diagnostics.ProcessStartInfo
  $extractInfo.FileName = $downloadPath
  $extractInfo.Arguments = '-y -o"{0}"' -f $stagingRoot
  $extractInfo.UseShellExecute = $false

  Write-Host "Extracting verified asset..."
  $extractProcess = [System.Diagnostics.Process]::Start($extractInfo)
  $extractProcess.WaitForExit()
  if ($extractProcess.ExitCode -ne 0) {
    throw "w64devkit extraction failed with exit code $($extractProcess.ExitCode)"
  }

  $compilerCandidates = @(
    Get-ChildItem $stagingRoot -Filter "g++.exe" -File -Recurse |
      Where-Object { $_.Directory.Name -eq "bin" }
  )
  if ($compilerCandidates.Count -ne 1) {
    throw "Expected one extracted bin\g++.exe; found $($compilerCandidates.Count)"
  }

  $extractedCompiler = $compilerCandidates[0].FullName
  $extractedVersion = Get-CompilerVersion $extractedCompiler
  if ($extractedVersion -ne $gccVersion) {
    throw "Expected extracted GCC $gccVersion; found $extractedVersion"
  }

  $extractedRoot = $compilerCandidates[0].Directory.Parent.FullName
  Move-Item $extractedRoot $cacheRoot

  [ordered]@{
    toolchain = "skeeto/w64devkit"
    toolchain_version = $toolchainVersion
    gcc_version = $gccVersion
    asset_url = $assetUrl
    asset_sha256 = $assetSha256
    signer_subject = $signature.SignerCertificate.Subject
  } | ConvertTo-Json | Set-Content -Encoding utf8 $markerPath

  $installedVersion = Get-CompilerVersion $compiler
  if ($installedVersion -ne $gccVersion) {
    throw "Installed compiler verification failed: expected $gccVersion; found $installedVersion"
  }

  Write-Host "w64devkit $toolchainVersion with GCC $gccVersion installed at $cacheRoot"
} finally {
  Remove-Item -Force $downloadPath -ErrorAction SilentlyContinue
  Remove-Item -Recurse -Force $stagingRoot -ErrorAction SilentlyContinue
}
