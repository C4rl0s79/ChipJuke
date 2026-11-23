# Audyt licencji libvgm pod kątem GPL-2.0-only

- Przedmiot: submoduł `external/libvgm`, commit `c5c03a2c2514d38d2ab7b7e8dbd021af0b8292cb` (2026-11-04).
- Cel: ustalić, które pliki libvgm można linkować do aplikacji na licencji **GPL-2.0-only**.
- Metoda: ręczny przegląd nagłówków (`license:`, `SPDX-License-Identifier`, `Copyright`, treść zastrzeżeń)
  oraz wyszukiwanie fraz (non-commercial, GPL, LGPL, BSD, MIT, public domain, „commercial”) we wszystkich
  plikach `emu/`, `player/`, `utils/`, `audio/`, `libs/`. Środowisko bez dostępu do sieci: nie sprawdzano
  upstreamów ani repozytoriów MAME/NSFPlay/Gens. Informacje o pochodzeniu spoza nagłówków plików są
  oznaczone „(z wiedzy, niezweryfikowane)”.
- To nie jest porada prawna. Pozycje „niejasny” wymagają decyzji właściciela projektu lub kontaktu z autorami.

## Zasady oceny zgodności z GPL-2.0-only

| Licencja pliku | Ocena | Uzasadnienie |
|---|---|---|
| BSD-3-Clause, MIT, zlib, domena publiczna | zgodny | Permisywne; kod można włączyć do całości na GPL-2.0. |
| GPL-2.0+ („or any later version”) | zgodny | Całość dystrybuowana jako GPL-2.0 spełnia warunki. |
| GPL-2.0 (bez „later”) | zgodny | Dokładnie ta wersja co aplikacja. Uwaga: uniemożliwia późniejszą zmianę całości na GPL-3.0. |
| LGPL-2.1+ | zgodny | LGPL 2.1 §3 pozwala zastosować do biblioteki zamiast LGPL zwykłą GPL v2 lub nowszą. |
| GPL-3.0-only / GPL-3.0+ (tylko „+” bez v2) | niezgodny | Brak. Nie znaleziono żadnego takiego pliku w libvgm. |
| Zastrzeżenie „non-commercial” | niezgodny | GPL §6 zabrania dodawania ograniczeń ponad GPL. |
| Brak nagłówka / licencji | niejasny | Brak udzielonej licencji; zgodność zależy od upstreamu lub od autora. |

## Ryzyko ogólne: brak pliku LICENSE w repozytorium libvgm

Repozytorium libvgm (ValleyBell/libvgm) **nie zawiera pliku LICENSE ani COPYING** (potwierdzone w tym
checkoucie: `find external/libvgm -iname "LICEN*" -o -iname "COPYING*"` nic nie zwraca; zgłoszenia upstream
#129 i #43 oraz PR #141 dotyczą tego tematu; stan ich rozstrzygnięcia nie został sprawdzony, brak sieci). Skutki:

1. Własny kod libvgm (Valley Bell: `player/*`, `utils/*`, `audio/*`, `emu/SoundEmu.c`, `emu/Resampler.c`,
   `emu/dac_control.c`, `emu/panning.c`, `emu/logging.c`, pliki `*intf.c`, `saa1099_vb.c`, `ymfmintf.cpp`)
   **nie ma żadnej deklaracji licencji w nagłówkach**. Domyślnie (prawo autorskie) bez licencji nikt nie ma
   prawa kopiować i rozpowszechniać. W praktyce projekt jest od lat używany i forkowany jako otwarty
   (wcześniejszy VGMPlay/libvgm bywa opisywany jako GPL, z wiedzy, niezweryfikowane), ale **to nie jest
   udzielona licencja**.
2. Rdzenie z jawnymi nagłówkami są objęte własnymi licencjami (tabela niżej), niezależnie od luki w
   repozytorium.
3. Rekomendacja: (a) przed pierwszym wydaniem poprosić autora (Valley Bell) o dodanie LICENSE/nagłówków
   (np. wskazać GPL-2.0-or-later lub dwuwarstwowo: własny kod + lista licencji rdzeni) i śledzić #129/#43/#141;
   (b) do tego czasu traktować wydanie jako obarczone ryzykiem i nie dystrybuować binariów poza testami;
   (c) zachować to ryzyko w `THIRD_PARTY_NOTICES.md` (zrobione: link do tego pliku).

## Podsumowanie

- Przejrzano wszystkie pliki źródłowe `emu/` (w tym 81 plików `.c/.cpp` w `emu/cores`), `player/`, `utils/`,
  `audio/`, `libs/`.
- **Nie znaleziono** plików na GPL-3.0-only ani na „MAME license” sprzed 2016: wszystkie rdzenie wywodzące się
  z MAME mają nagłówki `BSD-3-Clause` lub `GPL-2.0+` (po relicencjonowaniu MAME).
- **Niezgodny: 1 rdzeń** — `Ootake_PSG.c` (zakaz użycia komercyjnego). Jest on w libvgm domyślnym
  rdzeniem HuC6280. **Wyłączony** w konfiguracji CMake vgmplay (zastępuje go `c6280_mame.c`, BSD-3-Clause).
- **Niejasne (brak nagłówka licencji lub niepewne pochodzenie)**: `adlibemu_opl*` (kod Kena Silvermana
  pod nagłówkiem LGPL DOSBox; **wyłączony**), `np_nes_apu/dmc/fds.c` (NSFPlay), `sn76489.c` (Maxim),
  `ym2612.c` i `scd_pcm.c` (Gens), `qsound_ctr.c`, `ws_audio.c`, `ymdeltat.c`, `saa1099_vb.c`,
  `emu2413.c`/`emu2149.c` (brak tekstu licencji w pliku, upstream MIT), plus cały własny kod libvgm
  (brak LICENSE w repo).
- Zgodne z jawnym nagłówkiem: BSD-3-Clause (rdzenie MAME, ymfm), MIT (SameBoy, Mikey), GPL-2.0+ (MAME
  FM: fmopl/fmopn/ym2151/ym2413/ymf262/msm5232/nes_apu, Nuked OPLL, Mednafen VSU, Gens PWM),
  GPL-2.0 (ymf278b, openMSX), LGPL-2.1+ (Nuked OPL3/OPM/OPN2, DOSBox).
- Pliki dołączone w `libs/` (zlib 1.2.11, libiconv 1.15, nagłówki XAudio2) **nie są używane**: pod MSVC
  libvgm domyślnie wskazuje je jako zlib/iconv, więc konfiguracja vgmplay zeruje `ZLIB_ROOT`,
  `Iconv_INCLUDE_DIR`, `Iconv_LIBRARY` (zlib z vcpkg, konwersja stron kodowych przez WinAPI).
- `audio/` nie jest budowane (`BUILD_LIBAUDIO=OFF`).

## Lista plików problematycznych i propozycje

| # | Plik(i) | Problem | Propozycja | Status w konfiguracji |
|---|---|---|---|---|
| 1 | `emu/cores/Ootake_PSG.c`, `Ootake_PSG.h`, `Ootake_PSG_private.h` | Nagłówek (jap.): „użycie komercyjne zabronione; reszta wg GNU GPL”, obowiązek dołączania źródeł i powiadomienia autora. Dodatkowy zakaz komercyjny jest ograniczeniem ponad GPL (GPL §6). Część „Ki” (PSG.c) jest GPL-2.0+, ale całość pliku niezgodna. | **Wyłączyć rdzeń**, użyć `c6280_mame.c` (BSD-3-Clause). | `SNDEMU_C6280_OOTAKE=OFF` (wymuszone) |
| 2 | `emu/cores/adlibemu_opl2.c`, `adlibemu_opl3.c`, `adlibemu_opl_inc.c/.h`, `adlibemu.h` | Nagłówek LGPL-2.1+ (DOSBox Team) — zgodny — ale plik „originally based on ADLIBEMU.C by Ken Silverman, Copyright 1998-2001”. Oryginał Silvermana miał własne warunki użycia (z wiedzy: ograniczenia, w tym niekomercyjne; niezweryfikowane). DOSBox rozpowszechnia kod jako GPL, więc ryzyko jest prawdopodobnie niskie, ale nie zweryfikowane. | **Wyłączyć** (zamienniki: MAME `fmopl.c` GPL-2.0+, `ymf262.c`, Nuked OPL3 LGPL-2.1+). Alternatywnie zaakceptować po weryfikacji upstreamu DOSBox. | `SNDEMU_YM3812_ADLIBEMU=OFF`, `SNDEMU_YMF262_ADLIBEMU=OFF` |
| 3 | `emu/cores/np_nes_apu.c`, `np_nes_dmc.c`, `np_nes_fds.c` (+ nagłówki) | Port NSFPlay (Mamiya/Brezza), bez nagłówka licencji. Pochodzenie z NSFPlay; jego licencja nie jest zadeklarowana w plikach (z wiedzy, niezweryfikowane: zmienna historycznie). | **Zaakceptować warunkowo** albo wyłączyć (`VGMPLAY_LIBVGM_STRICT_LICENSES=ON`): wtedy rdzeń NES = MAME `nes_apu.c` (GPL-2.0+), **bez FDS** (brak alternatywy). Wyjaśnić z autorem libvgm/NSFPlay. | **Włączone** (domyślnie) |
| 4 | `emu/cores/sn76489.c`, `sn76489_private.h` | Maxim (2001–2002), modyfikacje Charlesa MacDonalda (SMS Plus); brak licencji w pliku. | Wyłączyć (zamiennik: `sn76496.c` MAME BSD-3-Clause) lub zaakceptować. | Włączone (wył. w trybie ostrożnym) |
| 5 | `emu/cores/ym2612.c`, `ym2612_int.h`, `scd_pcm.c` | Gens (Stéphane Dallongeville) z modyfikacjami Maxima/Blargga; brak tekstu licencji w pliku. Gens jest GPL-2.0+ (z wiedzy, niezweryfikowane), co byłoby zgodne. | Wyłączyć (zamienniki: `fmopn.c` GPGX GPL-2.0+, Nuked OPN2 `ym3438.c`; `rf5c68.c` MAME) lub zaakceptować. | Włączone (wył. w trybie ostrożnym) |
| 6 | `emu/cores/qsound_ctr.c` | Autor superctr; brak nagłówka licencji. | Wyłączyć (zamiennik `qsound_mame.c` BSD-3-Clause) lub poprosić autora o licencję. | Włączone (wył. w trybie ostrożnym) |
| 7 | `emu/cores/ws_audio.c`, `ws_initialIo.h` | WonderSwan; komentarz wspomina OSWAN; brak licencji w pliku. Pochodzenie: Oswan/Cygne (z wiedzy: GPL; niezweryfikowane). Brak alternatywy. | Zaakceptować i zweryfikować pochodzenie; jeśli się nie da, wyłączyć WonderSwan. | Włączone |
| 8 | `emu/cores/ymdeltat.c/.h` | MAME (Tatsuyuki Satoh, Burczynski); nagłówek bez tagu licencji (siostrzane `fmopl.c` ma GPL-2.0+). Wymagany przez YM2608/YM2610/Y8950. | Zaakceptować (prawdopodobnie GPL-2.0+ jak reszta rodziny FM). | Włączone |
| 9 | `emu/cores/fmopn.c` | Deklaruje GPL-2.0+; pochodzi z Genesis Plus GX (Eke-Eke) i MAME. Zgodny wg nagłówka; ostrożnie z historią licencji GPGX (część kodu GPGX miała klauzule niekomercyjne; z wiedzy, niezweryfikowane). | Zaakceptować (nagłówek jest jednoznaczny), zweryfikować upstream. | Włączone |
| 10 | `emu/cores/emu2413.c`, `emu2149.c` (+ `*_private.h`) | Brak tekstu licencji w pliku; upstream (digital-sound-antiques) to MIT (z wiedzy). Zgodne, o ile MIT. | Zaakceptować; zachować informację o autorze w NOTICES. | Włączone |
| 11 | `emu/cores/saa1099_vb.c`, `ymfmintf.cpp`, `*intf.c`, `emu/SoundEmu.c` i inne własne pliki Valley Bell | Brak licencji w plikach (patrz „Ryzyko ogólne”). | Uzyskać deklarację licencji od autora; do tego czasu ryzyko ogólne. | Włączone |
| 12 | `emu/cores/fmopn_2608rom.h`, `opll_*tone.h` | Dane: ADPCM ROM YM2608 (uzyskane z wyjścia układu; prawa Yamahy do oryginalnego ROM), tablice presetów (Okazaki, Chabin, Nuke.YKT). | Zaakceptować (praktyka wszystkich emulatorów); odnotować. | Włączone |
| 13 | `emu/cores/ymfm/*`, `ymfmintf.cpp` | BSD-3-Clause (Aaron Giles) — zgodne; wymaga C++14 (`REALLY_NEED_CXX14`). | Nie dotyczy (zgodny); rdzeń YM2414/OPZ pominięty ze względów kompilacji. | Wyłączone (`REALLY_NEED_CXX14=OFF`) |

## Tabela per plik: `emu/cores/*`

Legenda zgodności: **Z** zgodny, **N** niezgodny, **?** niejasny. „W buildzie” dotyczy konfiguracji domyślnej
vgmplay (`VGMPLAY_LIBVGM_STRICT_LICENSES=OFF`). Pliki `.h` dziedziczą ocenę swojego `.c`, o ile nie
zaznaczono inaczej (większość nagłówków to same deklaracje bez nagłówka licencji).

| Plik | Nagłówek (`license:` / copyright) | SPDX | Pochodzenie | GPL-2.0-only | W buildzie |
|---|---|---|---|---|---|
| `ay8910.c` | `BSD-3-Clause`, Couriersud | BSD-3-Clause | MAME | Z | tak |
| `bsmt2000.c` | `BSD-3-Clause`, Giles, Leaman, Horna, Mao, cam900 | BSD-3-Clause | MAME / PinMAME | Z | tak |
| `c140.c` | `BSD-3-Clause`, R. Belmont | BSD-3-Clause | MAME | Z | tak |
| `c219.c` | `BSD-3-Clause`, Belmont, superctr, Valley Bell | BSD-3-Clause | MAME | Z | tak |
| `c352.c` | `BSD-3-Clause`, Belmont, superctr | BSD-3-Clause | MAME | Z | tak |
| `c6280_mame.c` | `BSD-3-Clause`, Charles MacDonald | BSD-3-Clause | MAME | Z | tak |
| `Ootake_PSG.c`, `Ootake_PSG_private.h` | Kitao Nakamura 2006–2021 („użycie komercyjne zabronione”, „wg GNU GPL”), Ki 2004 (GPL-2.0+) | brak (niestandardowe) | Ootake (PC Engine) | **N** | **nie** |
| `es5503.c` | `BSD-3-Clause`, R. Belmont | BSD-3-Clause | MAME | Z | tak |
| `es5506.c` | brak (zaślepka 38 linii, bez rdzenia) | — | własny (Valley Bell) | ? (jak kod libvgm) | tak |
| `fmopl.c` | `GPL-2.0+`, Burczynski, Satoh | GPL-2.0-or-later | MAME | Z | tak |
| `fmopn.c` | `GPL-2.0+`, Burczynski, Satoh; wersja z Genesis Plus GX | GPL-2.0-or-later | MAME + Genesis Plus GX | Z (zob. uwaga 9) | tak |
| `gb_mame.c` | `BSD-3-Clause`, Pol, Kruize | BSD-3-Clause | MAME | Z | tak |
| `sameboy_apu.c` | `MIT`, Lior Halphon | MIT | SameBoy | Z | tak |
| `ics2115.c` | `BSD-3-Clause`, Marshall, nimitz, austere | BSD-3-Clause | MAME | Z | tak |
| `iremga20.c` | `BSD-3-Clause`, Tang, Belmont, Valley Bell | BSD-3-Clause | MAME | Z | tak |
| `k005289.c` | `BSD-3-Clause`, Bryan McPhail | BSD-3-Clause | MAME | Z | tak |
| `k007232.c` | SPDX `BSD-3-Clause`, Salmoria, Shioya, Mao, cam900 | BSD-3-Clause | MAME | Z | tak |
| `k051649.c` | `BSD-3-Clause`, Bryan McPhail | BSD-3-Clause | MAME | Z | tak |
| `k053260.c` | `BSD-3-Clause`, Corvi, Jackson | BSD-3-Clause | MAME | Z | tak |
| `k054539.c` | `BSD-3-Clause`, Olivier Galibert | BSD-3-Clause | MAME | Z | tak |
| `mikey.c` | `MIT`, laoo (backport C90: Valley Bell) | MIT | Atari Lynx (laoo) | Z | tak |
| `msm5205.c` | `BSD-3-Clause`, Aaron Giles (+ eito, cam900, VB, Mao) | BSD-3-Clause | MAME | Z | tak |
| `msm5232.c` | `GPL-2.0+`, Burczynski, Shioya, Salese (TA7630P fragment BSD-3) | GPL-2.0-or-later | MAME | Z | tak |
| `multipcm.c` | `BSD-3-Clause`, Miguel Angel Horna | BSD-3-Clause | MAME | Z | tak |
| `nes_apu.c`, `nes_defs.h` | `GPL-2.0+`, Matthew Conte (Nofrendo; „GNU Public License”) | GPL-2.0-or-later | MAME / Nofrendo | Z | tak |
| `np_nes_apu.c`, `np_nes_dmc.c`, `np_nes_fds.c` | brak; „portowane z NSFPlay 2.2/2.3 przez Valley Bell” | brak | NSFPlay | ? | tak |
| `nukedopl3.c`, `nukedopl3_int.h` | `LGPL-2.1+`, Nuke.YKT 2013–2020 | LGPL-2.1-or-later | Nuked OPL3 | Z | tak |
| `nukedopll.c`, `nukedopll_int.h` | `GPL-2.0+`, Nuke.YKT 2019–2023 | GPL-2.0-or-later | Nuked OPLL | Z | tak |
| `nukedopm.c`, `nukedopm_int.h` | `LGPL-2.1+`, Nuke.YKT 2020, 2026 | LGPL-2.1-or-later | Nuked OPM | Z | tak |
| `ym3438.c`, `ym3438_int.h` | `LGPL-2.1+`, Nuke.YKT (Alexey Khokholov) | LGPL-2.1-or-later | Nuked OPN2 | Z | tak |
| `okiadpcm.c` | `BSD-3-Clause`, Gardner, Giles | BSD-3-Clause | MAME | Z | tak |
| `okim6258.c` | `BSD-3-Clause`, Barry Rodewald | BSD-3-Clause | MAME | Z | tak |
| `okim6295.c` | `BSD-3-Clause`, Buffoni, Giles | BSD-3-Clause | MAME | Z | tak |
| `pokey.c` | `BSD-3-Clause`, Oliver, Smith, Buchmueller | BSD-3-Clause | MAME | Z | tak |
| `pwm.c` | GPL-2.0+ (tekst w nagłówku), Dallongeville, Akhoun, Korth | GPL-2.0-or-later | Gens / Gens/GS | Z | tak |
| `qsound_mame.c` | `BSD-3-Clause`, Leaman, Horna | BSD-3-Clause | MAME | Z | tak |
| `qsound_ctr.c` | brak; „by superctr (Ian Karlsson)” | brak | własny rdzeń superctr | ? | tak |
| `rf5c68.c` | `BSD-3-Clause`, Galibert, Giles | BSD-3-Clause | MAME | Z | tak |
| `scd_pcm.c` | brak; „część projektu Gens, Dallongeville” | brak (Gens: GPL-2.0+, niezweryfikowane) | Gens | ? | tak |
| `saa1099_mame.c` | `BSD-3-Clause`, Buchmueller, Abadia | BSD-3-Clause | MAME | Z | tak |
| `saa1099_vb.c` | brak; „Valley Bell, 2018” | brak | własny (Valley Bell) | ? (jak kod libvgm) | tak |
| `scsp.c`, `scspdsp.c`, `scsplfo.c` | `BSD-3-Clause`, ElSemi, R. Belmont | BSD-3-Clause | MAME | Z | tak |
| `segapcm.c` | `BSD-3-Clause`, Shioya, Galibert | BSD-3-Clause | MAME | Z | tak |
| `sn76489.c`, `sn76489_private.h` | brak; „Maxim 2001–2002, Charles MacDonald (SMS Plus)” | brak | Maxim / SMS Plus | ? | tak |
| `sn76496.c` | `BSD-3-Clause`, Nicola Salmoria | BSD-3-Clause | MAME | Z | tak |
| `upd7759.c` | `BSD-3-Clause`, Buchmueller, Balfour, Cohen, Galibert, Giles | BSD-3-Clause | MAME | Z | tak |
| `vsu.c` | GPL-2.0+ (tekst w nagłówku), „Mednafen” | GPL-2.0-or-later | Mednafen (Virtual Boy) | Z | tak |
| `ws_audio.c`, `ws_initialIo.h` | brak; wzmianka o OSWAN | brak | Oswan/Cygne (niezweryfikowane) | ? | tak |
| `x1_010.c` | `BSD-3-Clause`, Luca Elia | BSD-3-Clause | MAME | Z | tak |
| `ym2151.c` | `GPL-2.0+`, Burczynski, Corvi | GPL-2.0-or-later | MAME | Z | tak |
| `ym2413.c` | `GPL-2.0+`, Burczynski, Corvi | GPL-2.0-or-later | MAME | Z | tak |
| `ym2612.c`, `ym2612_int.h` | brak; „część projektu Gens”, mod. Maxim/Blargg | brak (Gens: GPL-2.0+, niezweryfikowane) | Gens | ? | tak |
| `ymdeltat.c` | brak tagu; Satoh, Burczynski | brak (MAME: GPL-2.0+, niezweryfikowane) | MAME | ? | tak |
| `ymf262.c` | `GPL-2.0+`, Burczynski | GPL-2.0-or-later | MAME | Z | tak |
| `ymf271.c`, `ymf271_opx.h` | `BSD-3-Clause`, superctr | BSD-3-Clause | MAME | Z | tak |
| `ymf278b.c` | `GPL-2.0` (bez „later”), „C-port of YMF278B.cc from openMSX” | GPL-2.0-only | openMSX | Z (wymusza GPL-2.0-only) | tak |
| `ymz280b.c` | `BSD-3-Clause`, Aaron Giles | BSD-3-Clause | MAME | Z | tak |
| `emu2149.c`, `emu2149_private.h` | brak tekstu; „Mitsutaka Okazaki 2001–2016” | brak (upstream MIT, niezweryfikowane) | emu2149 | ? (prawdop. Z) | tak |
| `emu2413.c`, `emu2413_private.h` | brak tekstu; „Copyright 2020 Mitsutaka Okazaki” | brak (upstream MIT, niezweryfikowane) | emu2413 | ? (prawdop. Z) | tak |
| `adlibemu_opl2.c`, `adlibemu_opl3.c`, `adlibemu_opl_inc.c/.h` | `LGPL-2.1+` (DOSBox Team); „na podstawie ADLIBEMU.C, Ken Silverman 1998–2001” | LGPL-2.1-or-later (+ Silverman bez SPDX) | DOSBox / Ken Silverman | ? | **nie** |
| `ymfm/ymfm.h`, `ymfm_fm.h`, `ymfm_fm.ipp`, `ymfm_opz.h`, `ymfm_opz.cpp` | BSD 3-Clause, Aaron Giles 2021 | BSD-3-Clause | ymfm | Z | **nie** (`REALLY_NEED_CXX14=OFF`) |
| `ymfmintf.cpp/.h` | brak | brak | własny (Valley Bell) | ? (jak kod libvgm) | nie |
| `2151intf.c`, `2413intf.c`, `2612intf.c`, `262intf.c`, `ayintf.c`, `c6280intf.c`, `gbintf.c`, `nesintf.c`, `oplintf.c`, `opnintf.c`, `qsoundintf.c`, `rf5cintf.c`, `saaintf.c`, `sn764intf.c` | brak (kod sklejający, Valley Bell) | brak | własny | ? (jak kod libvgm) | tak |
| `fmopn_2608rom.h`, `opll_2413tone.h`, `opll_281btone.h`, `opll_vrc7tone.h`, `emutypes.h` | dane / typy; autorzy w komentarzu | brak | patrz uwaga 12 | ? (dane) | tak |

Rdzeń `es5506.c` jest samą zaślepką (rejestruje urządzenie bez emulacji).

## Pozostałe katalogi libvgm

| Ścieżka | Nagłówek / licencja | Pochodzenie | GPL-2.0-only | W buildzie |
|---|---|---|---|---|
| `emu/*.c`, `emu/*.h` (SoundEmu, Resampler, dac_control, logging, panning, EmuStructs itd.) | brak | własny (Valley Bell) | ? (ryzyko ogólne) | tak |
| `player/*` (vgmplayer, s98player, droplayer, gymplayer, playerbase, playera, dblk_compr, helper) | brak | własny (Valley Bell) | ? (ryzyko ogólne) | tak |
| `utils/*` (DataLoader, FileLoader, MemoryLoader, StrUtils-CPConv_*, OS*) | brak | własny (Valley Bell) | ? (ryzyko ogólne) | tak (wersje Win) |
| `audio/*` (AudioStream, AudDrv_WASAPI/DSound/XAudio2/WinMM/WaveWriter/ALSA/OSS/Pulse/SADA/CoreAudio/libao) | brak | własny (Valley Bell) | ? (ryzyko ogólne) | **nie** (`BUILD_LIBAUDIO=OFF`; audio realizuje miniaudio) |
| `common_def.h`, `stdtype.h`, `_stdbool.h` | brak | własny | ? (ryzyko ogólne) | tak |
| `player.cpp`, `vgm2wav.cpp`, `audiotest.c`, `emutest.c`, `audemutest.c`, `vgmtest.c`, `vgm_dbcompr_bench.c` | brak | własny | ? | **nie** (programy, nie biblioteka) |
| `libs/include/zlib.h`, `zconf.h`, `libs/lib/zlib*.lib` | zlib (Gailly, Adler) — zgodny | zlib 1.2.11 | Z | **nie** (używamy zlib z vcpkg) |
| `libs/iconv/*` (`iconv.h`, `libiconv.lib`) | GNU libiconv: LGPL (nagłówek: „Library General Public License v2 lub nowsza”) | libiconv 1.15 | Z | **nie** (WinAPI) |
| `libs/include_vc/*`, `libs/include_mingw/*` (XAudio2.h, audiodefs.h, comdecl.h, xma2defs.h, sal_stub.h) | Copyright Microsoft Corporation, nagłówki SDK (własnościowe; warunki SDK) | Windows SDK / MinGW | ? | **nie** (audio nie jest budowane) |
| `libs/cmake_modules/*` | BSD-3-Clause (CMake) | CMake | Z | tylko jako moduły CMake (nie trafiają do binarium) |
| `cmake/*`, `CMakeLists.txt` | brak (`cmake/sanitizers`: moduły cmake-sanitizers, nie badano) | — | nie dotyczy binarium | nie wpływa na binarium |

## Zmiany w konfiguracji wynikające z audytu

Zob. główny `CMakeLists.txt`. Zrobione:

- `SNDEMU__ALL=OFF`, urządzenia włączane pojedynczo.
- `SNDEMU_C6280_OOTAKE=OFF` (rdzeń niezgodny), `SNDEMU_YM3812_ADLIBEMU=OFF` i `SNDEMU_YMF262_ADLIBEMU=OFF`
  (niejasne pochodzenie).
- Opcja `VGMPLAY_LIBVGM_STRICT_LICENSES` (domyślnie OFF) dodatkowo wyłącza rdzenie bez jawnej licencji,
  które mają zamiennik z jawną licencją (NSFPlay + FDS, Maxim SN76489, Gens YM2612 i RF5C164, QSound superctr).
- Uwaga dla kodu silnika: domyślne rdzenie wybierane przez libvgm (oznaczone `*` w `emu/CMakeLists.txt`) mogą
  być wyłączone w buildzie; silnik nie powinien zakładać obecności konkretnego rdzenia, tylko pytać
  `SndEmu_*` o dostępne i obsłużyć brak (błąd konkretny, nie crash).

## Decyzje wymagane od właściciela projektu

1. Czy zaakceptować NSFPlay (NES/FDS) i pozostałe rdzenie „?” w wydaniu, czy włączyć tryb ostrożny?
2. Czy kontaktować się z Valley Bell w sprawie LICENSE (zgłoszenia #129, #43, PR #141)? Do czasu odpowiedzi
   cała zależność ma status „licencja niejasna”.
3. Czy akceptować ograniczenie `ymf278b.c` (GPL-2.0-only) jako kolejny powód, by aplikacja pozostała
   GPL-2.0-only (nie „or later”)? (Zgodne z założoną licencją aplikacji.)
