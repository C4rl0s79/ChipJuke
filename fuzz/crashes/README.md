# Reproduktory znalezione w kampanii fuzzingu E5 (NIE naprawione - tylko zapis)

Pliki `*.vgm` odtwarzaja awarie znalezione przez `fuzz_decode` (libFuzzer + ASan, preset `clang-cl-asan-fuzz`).
Zadne `*_test.cpp` ich nie laduje (crash wywrocilby caly proces testowy). Po poprawce w `src/format` przenies plik do
`tests/data/` (i manifestu) albo do korpusu `fuzz/corpus/decode/` - wtedy test regresyjny `fuzz_regress_test`
sam sprawdzi, ze juz sie nie wywala.

Odtworzenie:
`out/build/clang-cl-asan-fuzz/fuzz/fuzz_decode.exe fuzz/crashes/<plik>`

| plik | skutek | miejsce |
|---|---|---|
| decode_gb_dmg_clock_1_div0.vgm | dzielenie przez zero: zegar GB DMG (0x80) = 1 => `smpl_rate = clock/32 = 0` | external/libvgm/emu/cores/sameboy_apu.c:2324 (`RC_SET_RATIO` w emu/RatioCntr.h:26) <- `device_start_gb_sameboy` <- `SndEmu_StartCore` <- `VGMPlayer::InitDevices` (vgmplayer.cpp:1640) <- `VgmDecoder::start` (src/engine/vgm_decoder.cpp:91) |
| decode_mikey_clock_1_div0.vgm | dzielenie przez zero: zegar Mikey (0xE4) = 1 | external/libvgm/emu/cores/mikey.c:973 `mikey_start` |
| decode_wswan_clock_14_div0.vgm | dzielenie przez zero: zegar WonderSwan (0xC0) = 14 => rate = clock/128 = 0 | external/libvgm/emu/cores/ws_audio.c:189 (`RC_SET_RATIO`) <- `ws_audio_init` <- `VGMPlayer::InitDevices` (vgmplayer.cpp:1596) |
| decode_slow_950B.vgm | NIE crash; pod ASan >10 s na 2 s audio (libFuzzer: slow-unit). Build Release MSVC (vgmrender): 2 s audio w 0,94 s (x2,1 realtime; typowy plik testowy x93) - nie jest realnym problemem (<1 s na 1 s audio), ale ok. 40x wolniejszy od typowego; przyczyna (rdzen) nieprofilowana | naglowek z zegarem YM2413, dane 0x8C... |

## Przyczyna wspolna (pierwsze trzy pliki)

`checkChipConfig` w `src/format/header.cpp` odrzuca zegar rowny 0 i zegar po przeliczeniu libvgm rowny 0, ale NIE
odrzuca zegarow, dla ktorych `clock / kRateDivisor[idx]` (czestotliwosc probkowania rdzenia) wynosi 0 - tablica
`kRateDivisor` jest juz w tym pliku. Kazdy rdzen libvgm uzywajacy `RC_SET_RATIO(&cntr, clock, rate)` dzieli
przez `rate`. Propozycja poprawki (do rozwazenia przez wlasciciela modulu `format`): odrzucic uklad, gdy
`kRateDivisor[idx] != 0 && eff / kRateDivisor[idx] == 0` (ErrorCode::BadHeader, offset = pole zegara). Klasa bledu jest
szersza niz trzy znalezione rdzenie - fuzzer trafil w nia trzy razy w ~12 min na rdzeniach, ktore akurat mutowal;
inne rdzenie z `RC_SET_RATIO`/dzieleniem przez rate sa tak samo narazone.

Tryb kampanii z pominieciem znanych bledow: `-DVGMPLAY_FUZZ_SKIP_OPEN_BUGS=ON` (filtr `hitsKnownOpenBug` w
`fuzz/fuzz_targets.hpp` pomija wejscia z niezerowym zegarem ukladu < 256 Hz). Po naprawie usun filtr.
