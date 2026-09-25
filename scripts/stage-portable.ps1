<#
.SYNOPSIS
  Stages a portable RayNeo Spatial folder from a build directory.

.DESCRIPTION
  Copies the controller, the renderer engine, the calibration tool, hidapi.dll,
  the shipped configuration and the notices into one folder that runs without
  the source tree. Nothing is installed, no driver is touched, no shortcut is
  created and nothing is pinned: the script only assembles a folder and prints
  what to do next.

  Layout of the staged folder:

    <Destination>\
      RayNeo Spatial.exe        controller (dashboard, editor, tray)
      spatial_desk.exe          renderer engine (D3D11 + Parsec VDD + capture)
      orientation_calibrate.exe guided sensor-to-head calibration
      hidapi.dll                HID transport used by the engine
      config\app.json           controller preferences (rewritten at runtime)
      config\layouts\*.json     layouts (default.json is the triple arc)
      config\layouts\presets\   named presets (triple, ultrawide; user presets are saved here)
      config\orientation.json   per-device calibration, copied only with -IncludeCalibration
      docs\*.md, README.md,
      LICENSE                   documentation, notices and licence
      scripts\create-start-menu-shortcut.ps1
      logs\                     created empty; engine log, telemetry CSV, status
      PORTABLE-NOTES.txt        how to run it

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\stage-portable.ps1
  powershell -ExecutionPolicy Bypass -File scripts\stage-portable.ps1 -BuildDir build\agent-X -Clean
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Destination = "dist\RayNeoSpatial",
    [switch]$IncludeSymbols,
    [switch]$IncludeCalibration,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath $repoRoot)) {
    throw "Could not resolve the repository root from '$PSScriptRoot'."
}

function Resolve-FullPath([string]$base, [string]$relative) {
    if ([System.IO.Path]::IsPathRooted($relative)) {
        return $relative
    }
    return (Join-Path $base $relative)
}

$buildPath = [System.IO.Path]::GetFullPath((Resolve-FullPath $repoRoot $BuildDir))
$destinationPath = [System.IO.Path]::GetFullPath((Resolve-FullPath $repoRoot $Destination))
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "dist"))

if (-not (Test-Path -LiteralPath $buildPath)) {
    throw "Build directory '$buildPath' does not exist. Configure and build first (see README.md)."
}

$controllerExe = Join-Path $buildPath "RayNeo Spatial.exe"
$engineExe = Join-Path $buildPath "spatial_desk.exe"
if (-not (Test-Path -LiteralPath $controllerExe)) {
    throw "'$controllerExe' is missing. Build the 'rayneo_spatial_app' target first."
}
if (-not (Test-Path -LiteralPath $engineExe)) {
    throw "'$engineExe' is missing. Build the 'spatial_desk' target first."
}

if ($Clean -and (Test-Path -LiteralPath $destinationPath)) {
    $distPrefix = $distRoot.TrimEnd('\') + '\'
    if (-not $destinationPath.StartsWith($distPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "-Clean is restricted to a child of '$distRoot'; refusing to remove '$destinationPath'."
    }
    Remove-Item -LiteralPath $destinationPath -Recurse -Force
}
$null = New-Item -ItemType Directory -Force -Path $destinationPath
$null = New-Item -ItemType Directory -Force -Path (Join-Path $destinationPath "config\layouts")
$null = New-Item -ItemType Directory -Force -Path (Join-Path $destinationPath "docs")
$null = New-Item -ItemType Directory -Force -Path (Join-Path $destinationPath "scripts")
$null = New-Item -ItemType Directory -Force -Path (Join-Path $destinationPath "logs")

$copied = New-Object System.Collections.ArrayList

function Copy-Required([string]$source, [string]$targetDirectory) {
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Required file '$source' is missing."
    }
    Copy-Item -LiteralPath $source -Destination $targetDirectory -Force
    $null = $copied.Add((Split-Path -Leaf $source))
}

function Copy-Optional([string]$source, [string]$targetDirectory) {
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $targetDirectory -Force
        $null = $copied.Add((Split-Path -Leaf $source))
        return $true
    }
    return $false
}

Copy-Required $controllerExe $destinationPath
Copy-Required $engineExe $destinationPath

$calibrationTool = Join-Path $buildPath "orientation_calibrate.exe"
if (Copy-Optional $calibrationTool $destinationPath) {
    Write-Host "Included orientation_calibrate.exe (guided calibration)."
} else {
    Write-Warning "orientation_calibrate.exe is not in '$buildPath'; head tracking cannot be calibrated from this folder."
}

$hidDll = Join-Path $buildPath "hidapi.dll"
if (-not (Copy-Optional $hidDll $destinationPath)) {
    Write-Warning "hidapi.dll was not found next to the executables. The engine will not start without it."
}

if ($IncludeSymbols) {
    foreach ($name in @("RayNeo Spatial.pdb", "spatial_desk.pdb")) {
        $null = Copy-Optional (Join-Path $buildPath $name) $destinationPath
    }
}

$layoutsSource = Join-Path $repoRoot "config\layouts"
if (Test-Path -LiteralPath $layoutsSource) {
    Get-ChildItem -LiteralPath $layoutsSource -Filter *.json -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destinationPath "config\layouts") -Force
        $null = $copied.Add("config\layouts\$($_.Name)")
    }
    $presetsSource = Join-Path $layoutsSource "presets"
    if (Test-Path -LiteralPath $presetsSource) {
        $presetsTarget = Join-Path $destinationPath "config\layouts\presets"
        $null = New-Item -ItemType Directory -Force -Path $presetsTarget
        Get-ChildItem -LiteralPath $presetsSource -Filter *.json -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $presetsTarget -Force
            $null = $copied.Add("config\layouts\presets\$($_.Name)")
        }
    }
} else {
    throw "config\layouts is missing from the repository; the engine needs at least default.json."
}

$appConfigSource = Join-Path $repoRoot "config\app.json"
if (Copy-Optional $appConfigSource (Join-Path $destinationPath "config")) {
    $null = $copied.Remove("app.json")
    $null = $copied.Add("config\app.json")
    $stagedConfig = Join-Path $destinationPath "config\app.json"
    try {
        $staged = Get-Content -LiteralPath $stagedConfig -Raw | ConvertFrom-Json
        $null = $staged.PSObject.Properties.Remove('last_engine_error')
        $null = $staged.PSObject.Properties.Remove('window')
        $stripped = $staged | ConvertTo-Json -Depth 8
        [System.IO.File]::WriteAllText($stagedConfig, $stripped,
            (New-Object System.Text.UTF8Encoding $false))
    } catch {
        Write-Warning "Could not strip developer state from the staged app.json: $_"
    }
} else {
    Write-Warning "config\app.json is missing; the controller will start from its built-in defaults."
}

$orientationSource = Join-Path $repoRoot "config\orientation.json"
if ($IncludeCalibration -and (Test-Path -LiteralPath $orientationSource)) {
    Copy-Item -LiteralPath $orientationSource -Destination (Join-Path $destinationPath "config") -Force
    $null = $copied.Add("config\orientation.json (per-device calibration)")
    Write-Host "Included the per-device calibration from config\orientation.json (-IncludeCalibration)."
} else {
    Write-Host "Skipped config\orientation.json: it is per-device hardware data that would silently"
    Write-Host "bias head tracking on other machines. Run orientation_calibrate.exe from the staged"
    Write-Host "folder instead (or re-stage with -IncludeCalibration to copy this machine's file)."
}

foreach ($document in @("README.md", "LICENSE", "docs\WINDOWS-APP.md", "docs\THIRD_PARTY_NOTICES.md")) {
    $source = Join-Path $repoRoot $document
    if (Test-Path -LiteralPath $source) {
        $target = Join-Path $destinationPath (Split-Path -Parent $document)
        $null = New-Item -ItemType Directory -Force -Path $target
        Copy-Item -LiteralPath $source -Destination $target -Force
        $null = $copied.Add($document)
    }
}

$shortcutHelper = Join-Path $repoRoot "scripts\create-start-menu-shortcut.ps1"
if (Test-Path -LiteralPath $shortcutHelper) {
    Copy-Item -LiteralPath $shortcutHelper -Destination (Join-Path $destinationPath "scripts") -Force
    $null = $copied.Add("scripts\create-start-menu-shortcut.ps1")
}

$notes = @"
RayNeo Spatial - portable folder
================================

Everything here runs from this folder; no source tree is required.

Run
---
1. Double-click "RayNeo Spatial.exe" (or pin it first, see below).
2. Set the glasses to Extend mode (Win+P -> Extend) and confirm the RayNeo
   display appears in Windows display settings.
3. Start workspace  -> creates the real Parsec virtual desktops (needs the
   signed Parsec VDD driver installed by you).
   Start preview    -> renderer-only: labelled test screens, no virtual
   desktops, no driver needed.
4. If orientation calibration is missing, use "Run calibration" in the
   Diagnostics panel; the tool opens in its own console window and writes
   config\orientation.json.

Pin (optional, manual)
----------------------
* From the running app: right-click its taskbar button -> Pin to taskbar.
* For the Start Menu: run scripts\create-start-menu-shortcut.ps1, then pin the
  new Start Menu entry (right-click -> Pin to Start / Pin to taskbar).
The app sets the AppUserModelID "RayNeo.Spatial.Desktop" so both routes give
one taskbar identity.

Logs
----
logs\engine.log          engine console output (rotated before each start)
logs\telemetry.csv       newest diagnostics CSV row shown in the dashboard
logs\engine-status.txt   engine state the dashboard polls twice per second

Notes
-----
* Nothing was installed and nothing was pinned by the staging script.
* The Parsec VDD driver is not bundled; install it separately if you want the
  full workspace.
* config\app.json is rewritten by the controller when it saves preferences.
"@
Set-Content -LiteralPath (Join-Path $destinationPath "PORTABLE-NOTES.txt") -Value $notes -Encoding UTF8

Write-Host ""
Write-Host "Staged '$destinationPath'"
Write-Host ("Copied: " + ($copied -join ", "))
Write-Host ""
Write-Host "Next steps (nothing was installed or pinned):"
Write-Host "  1. Start '$destinationPath\RayNeo Spatial.exe'"
Write-Host "  2. Read PORTABLE-NOTES.txt for Extend mode, calibration and VDD details"
Write-Host "  3. Optionally run scripts\create-start-menu-shortcut.ps1 -AppPath '<full path>'"
