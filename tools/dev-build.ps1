# Lokalny build: konfiguracja, kompilacja i testy dla wybranego presetu CMake.
# Uzycie (Windows PowerShell): powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev-build.ps1 [-Preset msvc-x64-debug] [-ConfigureOnly]
param([string]$Preset = 'msvc-x64-release', [switch]$ConfigureOnly)
$ErrorActionPreference = 'Stop'

# System32 i katalog instalatora VS (vswhere.exe) musza byc w PATH, inaczej
# Launch-VsDevShell nie znajdzie Windows SDK (rc.exe, mt.exe).
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$env:SystemRoot\System32;$installer;$env:PATH"

# Instancja VS z narzedziami C++ (Community, Build Tools lub inna).
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Nie znaleziono Visual Studio z narzedziami C++ (Microsoft.VisualStudio.Component.VC.Tools.x86.x64).' }
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

if (-not $env:VCPKG_ROOT) {
    if (Test-Path "$vs\VC\vcpkg\vcpkg.exe") { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
    else { throw 'Ustaw VCPKG_ROOT albo doinstaluj skladnik vcpkg w Visual Studio.' }
}

Set-Location (Split-Path -Parent $PSScriptRoot)
cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }
cmake --build --preset $Preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset $Preset
exit $LASTEXITCODE
