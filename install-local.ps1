$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$rundir = Join-Path $root "build_x64\rundir"
if (-not (Test-Path $rundir)) {
    throw "Build output not found at $rundir. Run cmake --build --preset windows-x64 first."
}

$config = @("RelWithDebInfo", "Release", "Debug") | Where-Object { Test-Path (Join-Path $rundir $_) } | Select-Object -First 1
if (-not $config) {
    throw "No built configuration found under $rundir"
}

$built = Join-Path $rundir $config
$dll = Get-ChildItem $built -Filter "gamepad-plugin.dll" | Select-Object -First 1
if (-not $dll) {
    throw "gamepad-plugin.dll not found in $built"
}

$localeSrc = Join-Path $root "data\locale\en-US.ini"
$pdb = Join-Path $built "gamepad-plugin.pdb"

function Install-To($binDir, $dataDir) {
    try {
        New-Item -ItemType Directory -Force -Path $binDir | Out-Null
        New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
        Copy-Item $dll.FullName (Join-Path $binDir "gamepad-plugin.dll") -Force
        if (Test-Path $pdb) {
            Copy-Item $pdb (Join-Path $binDir "gamepad-plugin.pdb") -Force
        }
        Copy-Item $localeSrc (Join-Path $dataDir "en-US.ini") -Force
        Write-Host "OK  $binDir"
        return $true
    } catch {
        Write-Host "SKIP $binDir ($($_.Exception.Message))"
        return $false
    }
}

Write-Host "Installing gamepad-plugin..."

# OBS 32 Windows actually loads from ProgramData, not AppData
Install-To (Join-Path $env:ProgramData "obs-studio\plugins\gamepad-plugin\bin\64bit") `
           (Join-Path $env:ProgramData "obs-studio\plugins\gamepad-plugin\data\locale") | Out-Null

# Legacy Program Files path (needs admin)
Install-To "C:\Program Files\obs-studio\obs-plugins\64bit" `
           "C:\Program Files\obs-studio\data\obs-plugins\gamepad-plugin\locale" | Out-Null

# AppData copy (not scanned by OBS 32 on Windows, kept for reference)
Install-To (Join-Path $env:APPDATA "obs-studio\plugins\gamepad-plugin\bin\64bit") `
           (Join-Path $env:APPDATA "obs-studio\plugins\gamepad-plugin\data\locale") | Out-Null

Write-Host ""
Write-Host "Fully quit OBS (check the tray) and reopen it."
Write-Host "Then: scene -> Add Source -> Gamepad"
Write-Host "Tools > Plugins is the plugin store. Custom sources do not appear there."
