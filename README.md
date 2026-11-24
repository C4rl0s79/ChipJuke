# vgmplay

<!-- Banner: docs/images/banner.png (to be added) -->

A tracker-style player for VGM / VGZ files on Windows. Chip emulation is provided by
[libvgm](https://github.com/ValleyBell/libvgm); the interface uses SDL3 and Dear ImGui (Direct3D 11),
audio output uses miniaudio (WASAPI).

Current version: **1.0.0-rc1** (release candidate). Licence: **GPL-2.0-only**.

Polish user guide: [docs/user-guide.pl.md](docs/user-guide.pl.md). Changes: [CHANGELOG.md](CHANGELOG.md).

## Features

- Plays VGM and gzip-compressed VGZ files, with loops (x2 and a 1 s fade-out by default; tunes without a loop end without a fade).
- Tracker view in a FastTracker 2 style: per-channel oscilloscopes with ProTracker-style VU meters, and a pattern grid
  with real notes, instruments and volumes decoded from the chip registers (SN76489, YM2612 incl. DAC, YM2151, YM2203, YM2413,
  YM3812/YM3526, YMF262, AY8910, NES APU/FDS, OKIM6295/6258, C140, K054539, K053260, SegaPCM, GA20). Other chips play normally
  but show no notes.
- Per-channel mute and solo.
- Playlists: several named playlists, M3U import/export, adding whole folders (scanned in the background), repeat Off/All/One,
  shuffle, session restore (same tune and position after a restart).
- Full keyboard, mouse and gamepad control, remappable in Settings.
- Audio output device selection.
- UI languages: English and Polish (F2). GD3 tags in English or Japanese (F3).

## Requirements

- Windows 10 22H2 or newer, 64-bit.
- A Direct3D 11 capable GPU/driver and an audio output device.
- Nothing to install: a single `vgmplay.exe`, no extra DLLs, no Visual C++ redistributable.

## Installation (portable)

1. Download `vgmplay-<version>-win64.zip` from the release page.
2. Optionally verify it: `Get-FileHash vgmplay-<version>-win64.zip -Algorithm SHA256` must match `SHA256SUMS.txt`.
3. Extract the ZIP anywhere (for example `D:\Tools\vgmplay`) and run `vgmplay.exe`.

There is no installer. To uninstall, delete the folder (and `%APPDATA%\vgmplay`, if it exists).
The `samples\` folder contains short synthetic test files made by the project's generator (not game rips).

## Where settings are stored

`settings.json`, `playlists.json` and `library-cache.json` are written **next to `vgmplay.exe`** when that
folder is writable (portable mode, no marker file needed). Otherwise (for example when extracted under
`C:\Program Files`) they go to `%APPDATA%\vgmplay`. `--settings-dir=<folder>` overrides both.

## Command line

```
vgmplay.exe [options] [file.vgm|file.vgz|folder ...]
```

Files and folders are added to the active playlist and played; with no arguments the last session is restored (paused).

| Option | Meaning |
|---|---|
| `--pl` | Start with the Polish interface (F2 toggles at runtime) |
| `--settings-dir=<folder>` | Use this folder for settings, playlists and metadata cache |
| `--allow-network-paths` | Allow UNC paths (`\server\share\...`) in playlists; rejected by default |
| `--no-autoplay` | Stop after loading instead of starting playback |
| `--null-audio` | No sound output (tests / CI) |
| `--diag` | Start with the diagnostics overlay (same as F12) |
| `--row-ticks=N` | Pattern row length in 44100 Hz ticks (735 = 1/60 s) |
| `--scopes-large` / `--scopes-hidden` | Initial oscilloscope strip height |

Developer-only diagnostics (`--fake-channel-scopes`, `--fake-pattern`) also exist.

## Controls

Keyboard (all of these can be remapped in Settings, F10, "Bindings"):

| Key | Action |
|---|---|
| Space | Play / pause |
| Esc | Stop (or close a message / go back) |
| P / N | Previous / next tune |
| Tab / Shift+Tab | Next / previous panel (Transport, Pattern, Playlist, Info) |
| Left / Right (Transport) | Seek +/-5 s |
| Up / Down (Transport) | Volume |
| `+` / `-` | Shorter / longer pattern row (1/120 ... 1/10 s) |
| M or Enter, S (Pattern) | Mute / solo the selected channel; 1..9,0 mute channels 1..10, Shift+1..0 solo |
| U / C / R (Pattern) | Unmute all / clear solo / reset both |
| Up/Down, Enter, Delete (Playlist) | Select, play, remove an entry; Ctrl+Up/Down moves it; Left/Right switches playlist |
| Ctrl+O / Ctrl+Shift+O | Add files / add folder |
| Ctrl+I / Ctrl+E | Import / export M3U |
| F2 | UI language PL/EN |
| F3 | GD3 tags English/Japanese |
| F4 | Oscilloscope strip small / large / hidden |
| F6 / F7 | Repeat Off/All/One / shuffle |
| F8 / F9 / Shift+Delete | New / rename / delete playlist |
| F10 | Settings |
| F12 | Diagnostics overlay |

Mouse: click the position bar to seek, drag the volume slider, wheel scrolls panels, double-click plays a playlist entry,
drag files or folders onto the window to add them, right click opens an entry menu.

Gamepad (Xbox layout; SDL maps other pads to it). The pad works only while the window has focus.

| Control | Action |
|---|---|
| D-pad / left stick | Navigate (with auto-repeat) |
| A / B | Confirm / back |
| X | Solo (Pattern) / remove entry (Playlist, with confirmation) |
| Y | Menu / settings |
| Start | Play / pause |
| LB / RB | Previous / next panel |
| LT / RT, or L3 / R3 | Previous / next tune |
| Right stick up/down | Page up / down |
| Select | Oscilloscope strip height |

## Known limitations

- Chips without a note decoder show a `...` column (sound is unaffected).
- Switching the audio device was not tested automatically on real hardware; if no output opens at start-up the player
  does not retry until the device is changed.
- Gamepad input (including hot-plug) and audio device switching can only be verified by hand on real hardware; report problems.
- Dialogs that open Windows file pickers (add files/folder, M3U) need a keyboard or mouse.
- Only trusted VGM/VGZ files should be opened; the loader is hardened and fuzz-tested, but this is a release candidate.
- Some libvgm cores have unclear upstream licences: see [Licensing](#licensing).

## Building from source

Requirements: Visual Studio 2022 (or Build Tools) with the C++ workload (MSVC x64, CMake, Ninja), Git, and
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```powershell
git clone --recurse-submodules https://github.com/C4rl0s79/VGMPlay vgmplay
cd vgmplay
# If cloned without --recurse-submodules:
git submodule update --init --recursive

powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev-build.ps1     # configure + build + test (Release)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1       # clean build + tests + portable ZIP
```

`tools\package.ps1` builds from scratch, runs the tests (aborts on failure), assembles `out\dist\vgmplay-<version>-win64\`,
checks it (no debug files, no non-system DLL dependencies) and writes a deterministic ZIP plus `SHA256SUMS.txt`.
Dependencies (SDL3, Dear ImGui, miniaudio, zlib, nlohmann-json) come from vcpkg (manifest mode, `vcpkg.json`,
triplet `x64-windows-static`, pinned by `builtin-baseline`). The version has one source: `project(VERSION)` and
`VGMPLAY_VERSION_SUFFIX` in `CMakeLists.txt`.

CMake options: `VGMPLAY_BUILD_TESTS`, `VGMPLAY_BUILD_FUZZ`, `VGMPLAY_STATIC_CRT`, `VGMPLAY_WARNINGS_AS_ERRORS`,
`VGMPLAY_LIBVGM_STRICT_LICENSES`.

## Source code (GPL-2.0)

The corresponding source for every binary release is the repository at the tag/commit recorded in the `SOURCE.txt`
file inside the ZIP. Repository: `https://github.com/C4rl0s79/VGMPlay`.

## Licensing

vgmplay is **GPL-2.0-only**; the full text is in [LICENSE](LICENSE). Third-party components and their licences:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the `licenses/` folder. libvgm contains emulation cores under
several licences and its own code has no licence file upstream; the audit and the decisions taken are in
[docs/licensing/libvgm-audit.md](docs/licensing/libvgm-audit.md).
