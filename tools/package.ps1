# Sklada przenosna paczke ZIP ChipJuke (Windows x64).
#
# Uzycie (Windows PowerShell):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild] [-NoSamples]
#                                                  [-Preset msvc-x64-release] [-SourceUrl <adres>]
#
#   (domyslnie)  konfiguracja i build Release OD ZERA (kasuje out/build/<preset>), testy, paczka
#   -NoBuild     uzywa istniejacego builda (np. z CI po osobnych krokach build + test); testy nie sa
#                wtedy uruchamiane ponownie
#   -SourceUrl   adres repozytorium zrodel wpisywany do SOURCE.txt (GPL-2.0: zrodla odpowiadajace
#                binarce). Domyslnie $env:VGMPLAY_SOURCE_URL, a gdy brak - https://github.com/C4rl0s79/ChipJuke.
#
# Wynik: out\dist\ChipJuke-<wersja>-win64\ , out\dist\ChipJuke-<wersja>-win64.zip , out\dist\SHA256SUMS.txt
# Skrypt przerywa przy bledzie budowy/testu/kontroli paczki.
param(
    [string]$Preset = 'msvc-x64-release',
    [switch]$NoBuild,
    [switch]$NoSamples,
    [string]$SourceUrl = $(if ($env:VGMPLAY_SOURCE_URL) { $env:VGMPLAY_SOURCE_URL } else { 'https://github.com/C4rl0s79/ChipJuke' })
)
$ErrorActionPreference = 'Stop'
$Placeholder = '<REPOSITORY-URL: uzupelnic przy publikacji / fill in at publication>'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail([string]$m) { Write-Host "BLAD: $m" -ForegroundColor Red; exit 1 }
function Step([string]$m) { Write-Host "== $m" -ForegroundColor Cyan }

# --- Srodowisko MSVC (jak w tools\dev-build.ps1; pomijane, gdy cl.exe juz w PATH, np. w CI) ---
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $env:PATH = "$env:SystemRoot\System32;$installer;$env:PATH"
    $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { Fail 'Nie znaleziono Visual Studio z narzedziami C++.' }
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    if (-not $env:VCPKG_ROOT) {
        if (Test-Path "$vs\VC\vcpkg\vcpkg.exe") { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
    }
}
if (-not $env:VCPKG_ROOT -and -not $NoBuild) { Fail 'Ustaw VCPKG_ROOT.' }
Set-Location $root   # Launch-VsDevShell mogl zmienic katalog

$buildDir = Join-Path $root "out\build\$Preset"

# --- Build od zera + testy ---
if (-not $NoBuild) {
    Step "Build od zera ($Preset)"
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
    cmake --preset $Preset;            if ($LASTEXITCODE -ne 0) { Fail 'cmake configure' }
    cmake --build --preset $Preset;    if ($LASTEXITCODE -ne 0) { Fail 'cmake build' }
    Step 'Testy'
    ctest --preset $Preset;            if ($LASTEXITCODE -ne 0) { Fail 'testy nie przechodza - paczka NIE powstaje' }
}

$exe = Join-Path $buildDir 'src\app\ChipJuke.exe'
if (-not (Test-Path $exe)) { Fail "Brak $exe (zbuduj projekt albo uzyj bez -NoBuild)." }

# --- Wersja z wygenerowanego naglowka (jedno zrodlo prawdy: CMake) ---
$hdr = Get-Content -Raw (Join-Path $buildDir 'generated\vgmplay\version.hpp')
function HdrValue([string]$name) {
    if ($hdr -match "$name\s*=\s*`"([^`"]*)`"") { return $Matches[1] }
    Fail "Brak $name w version.hpp"
}
$version = HdrValue 'kVersionFull'
$commit  = HdrValue 'kGitCommit'
$dirty   = $hdr -match 'kGitDirty\s*=\s*1'
if ($dirty) { Write-Warning 'Drzewo robocze ma niezatwierdzone zmiany - paczka nie odpowiada zadnemu commitowi.' }

# Wersja w exe musi zgadzac sie z naglowkiem.
$fv = (Get-Item $exe).VersionInfo
if ($fv.ProductVersion -ne $version) { Fail "ProductVersion exe ($($fv.ProductVersion)) != $version (przebuduj od zera)." }

$name     = "ChipJuke-$version-win64"
$distRoot = Join-Path $root 'out\dist'
$stage    = Join-Path $distRoot $name
$zip      = Join-Path $distRoot "$name.zip"
Step "Skladanie $name (commit $commit)"
New-Item -ItemType Directory -Force $distRoot | Out-Null
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip)   { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

function CopyTo([string]$from, [string]$dstRel) {
    if (-not (Test-Path $from)) { Fail "Brak pliku do paczki: $from" }
    $dst = Join-Path $stage $dstRel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Copy-Item $from $dst
}

CopyTo $exe 'ChipJuke.exe'
foreach ($f in 'LICENSE','README.md','CHANGELOG.md','THIRD_PARTY_NOTICES.md') { CopyTo (Join-Path $root $f) $f }
CopyTo (Join-Path $root 'docs\user-guide.pl.md') 'docs\user-guide.pl.md'
# licenses\ - teksty licencji skladnikow + audyt libvgm + licencja fontu.
Get-ChildItem (Join-Path $root 'licenses') -File | ForEach-Object { CopyTo $_.FullName "licenses\$($_.Name)" }
CopyTo (Join-Path $root 'docs\licensing\libvgm-audit.md') 'licenses\libvgm-audit.md'
CopyTo (Join-Path $root 'assets\fonts\OFL.txt') 'licenses\BIZUDGothic-OFL.txt'
# Tryb przenosny nie wymaga znacznika: settings.json powstaje obok exe, gdy katalog jest zapisywalny
# (src/settings: resolveSettingsLocation), inaczej w %APPDATA%\ChipJuke. Dlatego brak portable.txt.

# samples\ - syntetyczne pliki z generatora (tools\vgmgen), korpus tests\data\valid (wersjonowany w git).
if (-not $NoSamples) {
    Get-ChildItem (Join-Path $root 'tests\data\valid') -File | ForEach-Object { CopyTo $_.FullName "samples\$($_.Name)" }
}

# SOURCE.txt - GPL-2.0: skad wziac zrodla odpowiadajace tej binarce.
$subCommit  = (git -C $root rev-parse HEAD:external/libvgm).Trim()
$fullCommit = (git -C $root rev-parse HEAD).Trim()
$srcUrl = if ($SourceUrl) { $SourceUrl } else { $Placeholder }
$dirtyNote = if ($dirty) { ' (+ uncommitted changes)' } else { '' }
$srcText = @"
ChipJuke $version - source code / kod zrodlowy
=========================================

ChipJuke is licensed under GPL-2.0-only (see LICENSE). The complete corresponding source code
for this binary is available at:

  Repository : $srcUrl
  Commit     : $fullCommit$dirtyNote
  Submodule  : external/libvgm @ $subCommit
  Build      : tools\package.ps1 (MSVC, vcpkg triplet x64-windows-static; versions in vcpkg.json)

Pelny kod zrodlowy odpowiadajacy tej binarce jest dostepny pod powyzszym adresem i commitem.
Third-party components and their licenses: THIRD_PARTY_NOTICES.md and licenses\.
"@
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $stage 'SOURCE.txt'), ($srcText -replace "`r?`n", "`r`n") + "`r`n", $utf8)
if (-not $SourceUrl) { Write-Warning "SourceUrl nie podano - SOURCE.txt zawiera PLACEHOLDER: $Placeholder" }

# --- Kontrole paczki ---
Step 'Kontrole'
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.pdb','.ilk','.exp','.lib','.obj','.dll' }
if ($bad) { Fail ("Niepozadane pliki w paczce: " + ($bad.Name -join ', ')) }

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) { Fail 'Brak dumpbin.exe (srodowisko MSVC).' }
$deps = @(& dumpbin.exe /nologo /dependents (Join-Path $stage 'ChipJuke.exe') |
    ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } } | Sort-Object -Unique)
if ($deps.Count -eq 0) { Fail 'dumpbin nie zwrocil zadnych zaleznosci - nieoczekiwany wynik.' }
Write-Host 'Zaleznosci DLL ChipJuke.exe:'
$deps | ForEach-Object { Write-Host "  $_" }
$sysdir = Join-Path $env:SystemRoot 'System32'
$nonSystem = @()
foreach ($d in $deps) {
    # DLL systemowa: istnieje w System32, albo to api-set (api-ms-win-*, ext-ms-win-*).
    if ($d -match '^(api|ext)-ms-win-') { continue }
    if (-not (Test-Path (Join-Path $sysdir $d))) { $nonSystem += $d }
}
$crt = @($deps | Where-Object { $_ -match '^(vcruntime|msvcp|concrt|ucrtbase)' })
if ($nonSystem.Count -gt 0) { Fail ("DLL spoza systemu Windows: " + ($nonSystem -join ', ')) }
if ($crt.Count -gt 0)       { Fail ("Exe zalezy od dynamicznego CRT: " + ($crt -join ', ')) }

$exeSize = (Get-Item (Join-Path $stage 'ChipJuke.exe')).Length
Write-Host ("Rozmiar ChipJuke.exe: {0:N0} B ({1:N2} MiB)" -f $exeSize, ($exeSize / 1MB))

# --- ZIP deterministyczny: stala kolejnosc wpisow (porzadkowa), stala data, stale atrybuty ---
Step 'ZIP'
Add-Type -AssemblyName System.IO.Compression
$rels = [string[]]@(Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) -replace '\\', '/' })
[Array]::Sort($rels, [System.StringComparer]::Ordinal)
$fixedTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$fs = [System.IO.File]::Create($zip)
try {
    $za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($rel in $rels) {
            $entry = $za.CreateEntry("$name/$rel", [System.IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTime
            $es = $entry.Open()
            try {
                $b = [System.IO.File]::ReadAllBytes((Join-Path $stage ($rel -replace '/', '\')))
                $es.Write($b, 0, $b.Length)
            } finally { $es.Dispose() }
        }
    } finally { $za.Dispose() }
} finally { $fs.Dispose() }

# --- SHA256SUMS.txt (format sha256sum: "<hash> *<plik>") ---
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$sums = Join-Path $distRoot 'SHA256SUMS.txt'
[System.IO.File]::WriteAllText($sums, "$hash *$name.zip`n", $utf8)

$zipSize = (Get-Item $zip).Length
$dirtyTag = if ($dirty) { ' (dirty)' } else { '' }
Write-Host ''
Write-Host "ZIP    : $zip"
Write-Host ("Rozmiar: {0:N0} B" -f $zipSize)
Write-Host "SHA256 : $hash"
Write-Host "Sumy   : $sums"
Write-Host "Commit : $commit$dirtyTag"
