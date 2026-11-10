# Better Key installer for OBS Studio (Windows x64)
# Copies the plugin into %PROGRAMDATA%\obs-studio\plugins\better-key
# Run: powershell -ExecutionPolicy Bypass -File install.ps1

$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot 'better-key'
if (-not (Test-Path $src)) {
    Write-Error "Payload folder not found: $src`nExtract the release zip first, then run this script."
    exit 1
}

$pluginsDir = Join-Path $env:PROGRAMDATA 'obs-studio\plugins'
$dest = Join-Path $pluginsDir 'better-key'

# stop obs if it's holding the dll
$obs = Get-Process obs64 -ErrorAction SilentlyContinue
if ($obs) {
    Write-Host "OBS is running - closing it first..."
    $obs | Stop-Process -Force
    Start-Sleep -Seconds 2
}

# remove legacy install name if present
$legacy = Join-Path $pluginsDir 'infinity-key'
if (Test-Path $legacy) {
    Write-Host "Removing old 'infinity-key' install..."
    Remove-Item $legacy -Recurse -Force
}

New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $src '*') $dest -Recurse -Force

Write-Host ""
Write-Host "Better Key installed to $dest"
Write-Host "Restart OBS, then add Filters -> 'Better Key' to your source."
