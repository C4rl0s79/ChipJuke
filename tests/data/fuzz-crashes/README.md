# Reproduktory awarii znalezione/odtworzone przy fuzzingu (nie poprawiane w E1 - tylko zapis)

Pliki `known_*.vgm` odtwarzaja ZNANE awarie procesu z przegladu bezpieczenstwa. Kazdy przechodzi dzis
`format::parseVgmHeader` + `validateCommandStream` (loader go przyjmuje), a wywala sie w libvgm. Odpowiedzialny:
backend-implementer (modul format - dodac kontrole w pre-skanie). NIE sa czescia manifestu ani testow (zadne
`*_test.cpp` ich nie laduje), bo test wywalilby caly proces.

Odtworzenie (preset `clang-cl-asan-fuzz`, patrz `fuzz/CMakeLists.txt`):
`out/build/clang-cl-asan-fuzz/fuzz/fuzz_decode.exe tests/data/fuzz-crashes/<plik>`

| plik | skutek | miejsce |
|---|---|---|
| known_c352_divider_zero.vgm | dzielenie przez zero (bajt naglowka 0xD6 = 0 przy C352) | external/libvgm/player/vgmplayer.cpp:1374 (GenerateDeviceConfig) |
| known_dac_start_msec_without_freq.vgm | dzielenie przez zero: 0x93 w trybie MSEC bez 0x92 | emu/dac_control.c:528 (daccontrol_start) |
| known_dac_start_toend_without_data.vgm | dzielenie przez zero: 0x93 tryb TOEND bez skutecznego 0x91 | emu/dac_control.c:534 (daccontrol_start) |
| known_pokey_huge_clock.vgm | zegar POKEY 0x3FFFFFFF => malloc ~8 GB => abort() w Resmpl_EnsureBuffers | libFuzzer: out-of-memory (malloc 8589934584) |

Uwaga: warianty 0x93 tryb BYTES bez danych i 0x95 bez danych NIE wywalaja sie w tej wersji libvgm (sprawdzone).
Pliki zbudowano skryptem recznie (naglowek 0x100 B + kilka komend), nie wyszly z fuzzera: 60 s na cel
(patrz docs/testing/e1-test-plan.md) nie trafilo na nie samodzielnie.
