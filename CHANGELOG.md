# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/). Versioning: [Semantic Versioning](https://semver.org/).
Pre-release suffixes (`-rc1`) are stored in `VGMPLAY_VERSION_SUFFIX` in `CMakeLists.txt`.

## [1.0.0-rc3] - 2026-11-25

### Added
- Note decoding for **Konami SCC (K051649 / K052539)**: 5 wavetable channels, pitch from the period, waveform as the
  instrument, shared channel 4/5 waveform. MSX music (Metal Gear 2, Space Manbow) showed `b/d` for SCC channels.
- Note decoding for **Y8950 (MSX-AUDIO)**: FM and rhythm as YM3526, plus the ADPCM channel (start/stop, relative pitch
  from delta-N, sample end).
- Note decoding for **QSound** (CPS2): 16 PCM voices, key-on on a 0 -> non-zero frequency or the key-on register,
  bank, volume, sample end; the 3 ADPCM channels stay without notes.
- `tools/libscan/libscan.py`: scans a VGM library and lists which chips and special modes (OPL rhythm, OPL3 4-op,
  YM2612 DAC / CH3 special, ADPCM, ...) each game uses, with example tracks for testing.
- **OPL4 (YMF278B, Moonsound)**: the external sample ROM `yrw801.rom` is loaded from the folder of `ChipJuke.exe` or
  of the track (not included - Yamaha copyright). Without it the Info panel warns that ROM instruments may be silent;
  previously the ROM could not be loaded at all.
- `vgmrender --core CHIP=FCC` (e.g. `--core YM2612=NUKE`) renders with a chosen emulation core, for A/B comparisons;
  `--list-cores` now prints chip names.

### Fixed
- **NES FDS** channel showed no notes in files that never write `$4023` (e.g. Zelda no Densetsu FDS); the decoder now
  starts with the FDS sound I/O enabled, as libvgm does on reset.
- **YMF262 (OPL3) 4-operator mode**: per-channel oscilloscopes and mute/solo did not match the notes. The note sits on
  the first channel of a 4-op pair, but the emulator outputs the pair through the second one; the second channel now
  follows the first channel's mute state (Dark Seraphim).
- GD3 line breaks and tabs (multi-line "Notes" on vgmrips, e.g. Out Run) no longer trigger the
  "control characters removed from tags" warning; they are still shown as spaces.

## [1.0.0-rc2] - 2026-11-24

### Added
- Note decoding for **YM2608 (OPNA)** and **YM2610 / YM2610B (OPNB)**: FM channels (incl. CH3 special mode and the
  YM2608 prescaler), SSG (AY-compatible), YM2608 rhythm, ADPCM-A and ADPCM-B. PC-9801 and Neo Geo music now shows
  notes in the pattern (reported with Brandish 3 for PC-9801, which previously showed only `...`).

### Verified
- Note pitch matches the libvgm output for YM2608, YM2610, YM2203 and YM2612 (FFT, ratio 0.9999-1.0002).
- No change in decoded notes for YM2612 and YM2203 files.

## [1.0.0-rc1] - 2026-11-24

First release candidate. Distribution: portable ZIP only (Windows 10 22H2+, x64, single `ChipJuke.exe`).

### Added
- Playback of VGM and VGZ (libvgm): loops (x2 and 1 s fade-out by default), tunes without a loop end without a fade;
  GD3 tags (English/Japanese), file information panel, L/R oscilloscope.
- Tracker view in a FastTracker 2 layout: per-channel oscilloscopes with ProTracker-style VU meters and a pattern with
  real notes, instruments and volumes decoded from chip registers (SN76489, YM2612 + DAC, YM2151, YM2203, YM2413,
  YM3812/YM3526, YMF262, AY8910, NES APU/FDS, OKIM6295/6258, C140, K054539, K053260, SegaPCM, GA20); YM2203/YM2612
  channel 3 special mode and operator retrigger handled. Per-channel mute/solo.
- Playlists: multiple named playlists, M3U import/export, folder scan in the background, Prev/Next, automatic advance,
  repeat Off/All/One, shuffle, metadata (title, length) filled in after load, session restore.
- Settings (F10): loops, fade, behaviour at end without loop, volume, language, GD3 language, pattern row length,
  audio output device, network paths, button hints. Stored next to the executable (portable) or in `%APPDATA%\ChipJuke`.
- Full gamepad control of the whole application, button hints bar, remappable keyboard and gamepad bindings with
  conflict handling and restore of defaults.
- UI in English and Polish; `--settings-dir`, `--pl`, `--allow-network-paths` and other command-line options.
- Portable ZIP packaging (`tools/package.ps1`), version information in the executable (VERSIONINFO) and a generated
  `vgmplay/version.hpp`, release workflow (`.github/workflows/release.yml`), user guide (`docs/user-guide.pl.md`),
  third-party licence texts (`licenses/`).
- `vgmrender` command-line tool (offline render of VGM to WAV with the same decoder as the player).
- Test corpus of synthetic VGM/VGZ files with a generator (`tools/vgmgen`), golden-audio tests, fuzz targets.

### Security
- Hardening after security reviews of every stage: bounds and limits in the VGM/GD3 parser and loader (C352, DAC streams,
  clocks, EOF offset, GD3), pipes, large playlists, UNC/network paths rejected by default, settings file handling,
  metadata scan performance, bindings and audio device validation.

### Known issues
- Chips without a note decoder show an "n/a" column; audio is unaffected.
- Audio device switching and gamepad hot-plug were not tested automatically on real hardware.
- Licences of some libvgm cores are not declared upstream (see `docs/licensing/libvgm-audit.md`).
  Enabled by decision: NSFPlay-derived NES cores, Maxim SN76489, Gens YM2612/SCD PCM, superctr QSound.

[1.0.0-rc2]: https://github.com/C4rl0s79/ChipJuke/releases/tag/v1.0.0-rc2
[1.0.0-rc1]: https://github.com/C4rl0s79/ChipJuke/releases/tag/v1.0.0-rc1
