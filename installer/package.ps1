# Better Key release packager
# Builds RelWithDebInfo, stages the plugin payload + installer,
# and zips it as release/better-key-windows-x64.zip
# Run: powershell -ExecutionPolicy Bypass -File installer/package.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

Write-Host "==> Configuring + building"
cmake --preset windows-x64
cmake --build "$root/build_x64" --config RelWithDebInfo

$rundir = "$root/build_x64/rundir/RelWithDebInfo"
$dll = Get-ChildItem $rundir -Recurse -Filter 'better-key.dll' |
    Select-Object -First 1
if (-not $dll) { Write-Error "better-key.dll not found under $rundir"; exit 1 }

$stage = "$root/release/stage"
$payload = "$stage/better-key"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$payload/data/locale" | Out-Null

Copy-Item $dll.FullName "$payload/better-key.dll"
Copy-Item "$root/data/better_key.effect" "$payload/data/better_key.effect"
Copy-Item "$root/data/locale/*" "$payload/data/locale/" -Recurse
Copy-Item "$root/installer/install.ps1" "$stage/install.ps1"
Copy-Item "$root/LICENSE" "$stage/LICENSE.txt"
Copy-Item "$root/README.md" "$stage/README.md"

$zip = "$root/release/better-key-windows-x64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage/*" -DestinationPath $zip

Write-Host ""
Write-Host "==> Packaged: $zip"
Get-ChildItem $payload -Recurse | ForEach-Object { Write-Host "   $($_.FullName.Substring($stage.Length))" }
