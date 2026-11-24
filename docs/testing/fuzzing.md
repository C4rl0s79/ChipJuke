# Fuzzing (libFuzzer + ASan, clang-cl)

Cele w `fuzz/`; ciala celow sa w jednym naglowku `fuzz/fuzz_targets.hpp` (funkcje `run*` zwracajace `false` przy
naruszeniu niezmiennika). Uzywaja ich cienkie `fuzz_*.cpp` (libFuzzer) oraz `tests/fuzz_regress_test.cpp` (doctest, MSVC).

| Cel | Co testuje | Slownik | Ziarna |
|---|---|---|---|
| fuzz_vgm_header | gzip -> naglowek -> GD3 -> strumien | vgm.dict | tests/data, pliki z D:\emu\vgm |
| fuzz_gd3 | parseGd3 | vgm.dict | j.w. |
| fuzz_gzip | decompressIfGzip | vgm.dict | j.w. |
| fuzz_stream | parseVgmHeader + validateCommandStream | vgm.dict | j.w. |
| fuzz_decode | loader + render ~2 s przez libvgm | vgm.dict | j.w. |
| fuzz_pattern | loader + chipstate::buildPattern | vgm.dict | j.w. |
| fuzz_settings | settings::fromJson (+ idempotencja toJson) | settings.dict | `fuzz/make_seeds.py` |
| fuzz_playlist_json | playlist::loadJson (+ idempotencja saveJson) | playlist.dict | make_seeds.py |
| fuzz_m3u | playlist::importM3u (4 warianty opcji) + exportM3u | m3u.dict | make_seeds.py |
| fuzz_library_cache | MetaCache::fromJson (+ toJson) | library_cache.dict | make_seeds.py |

## Budowa i uruchomienie

```
set VGMPLAY_CLANG_RT_DIR=<VS BuildTools>\VC\Tools\Llvm\x64\lib\clang\<wersja>\lib\windows
cmake --preset clang-cl-asan-fuzz [-DVGMPLAY_FUZZ_SKIP_OPEN_BUGS=ON]
cmake --build --preset clang-cl-asan-fuzz
fuzz_settings.exe -max_total_time=600 -jobs=2 -workers=2 -dict=fuzz/settings.dict <korpus_roboczy> <ziarna>
```

Wymaga PATH z `clang-cl` i `clang_rt.asan_dynamic-x86_64.dll` (katalog jak wyzej). Moduly vgmplay i libvgm sa
kompilowane z `-fsanitize=fuzzer-no-link` (inaczej libFuzzer nie widzi pokrycia poza celem).
`VGMPLAY_FUZZ_SKIP_OPEN_BUGS=ON` pomija wejscia z zegarami ukladow < 256 Hz (znany blad dzielenia przez zero,
`fuzz/crashes/README.md`); po naprawie usunac filtr `hitsKnownOpenBug`.

## Korpus regresyjny

`fuzz/corpus/<cel>/` (<= 200 plikow, <= 2 MB na cel) - zminimalizowany zbior z kampanii
(`-set_cover_merge=1 -max_len=<limit>`). `tests/fuzz_regress_test.cpp` odtwarza go na zwyklym buildzie MSVC.
Reproduktory znanych awarii: `fuzz/crashes/` (NIE sa w tescie, dopoki bledy nie sa naprawione).
