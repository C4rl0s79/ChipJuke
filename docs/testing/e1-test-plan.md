# Plan testow E1 (format, loader, silnik - korpus syntetyczny)

Status: PLAN. Testy C++ powstaja PO scaleniu galezi `e1-corpus` (dane) z galeziami format/loader
i silnika. Ten dokument opisuje, co i jak testowac na korpusie z `tests/data/`
(opis korpusu i manifestu: `tests/data/README.md`; generator: `tools/vgmgen/`).

## 1. Zasady

- Oczekiwania pochodza z `tests/data/manifest.json` (wczytywane w tescie; stala
  `VGMPLAY_TEST_DATA_DIR` jest juz zdefiniowana w `tests/CMakeLists.txt`). Test nie powtarza
  liczb z manifestu na sztywno.
- Wpis z `expect.alternatives` akceptuje wynik podstawowy lub alternatywny, ale test ZAPISUJE,
  ktory zaszedl (wypisanie przez `INFO`/log), zeby niejednoznacznosci kontraktu zamykac swiadomie.
  Zamkniecie = usuniecie alternatywy z generatora, nie rozluznienie testu.
- Zadnego usuwania/oslabiania przypadkow, zeby uzyskac zielony wynik; niespelnione oczekiwanie to
  albo blad implementacji, albo (po decyzji) poprawka generatora + manifestu.
- Dla `.vgz` nie porownuj SHA-256 pliku (zalezy od wersji zlib), tylko `rawSha256`/zawartosc.
- Testy czyste (format) bez I/O poza wczytaniem bajtow pliku; loader testowany przez
  `loadTrackFromMemory` i (jeden test) `loadTrackFromFile`.

## 2. Testy jednostkowe (doctest, `vgmplay_tests`)

### 2.1 `format_corpus_test.cpp` - na wszystkich wpisach manifestu

Dla kazdego `files[]` (z uwzglednieniem `limits`):

1. `decompressIfGzip` -> `parseVgmHeader` -> `parseGd3` -> `validateCommandStream`
   (lancuch jak w loaderze) i porownanie z `expect`:
   - `result=error`: kod bledu rowny `expect.error` (lub alternatywie); `Error::byteOffset`
     (jesli ustawiony) < rozmiar danych; komunikat niepusty.
   - `result=ok`: brak bledu, zbior `warnings` zawiera `expect.warnings` (rownosc zbiorow dla
     plikow `valid/`: puste).
2. Dla `ok`: `header.*`, `chips[]` (po `(type, instance)`), `stream.*` zgodne z manifestem;
   `stream.totalSamples` to suma opoznien, NIE `header.totalSamples` (plik
   `sn76489_header_total_lies_v150.vgm`).
3. Dla `gd3`: pola rowne co do bajtu UTF-8 (w tym japonskie); dla `gd3_*` z `...Contains`,
   `mustBeValidUtf8` (wlasny walidator UTF-8 w tescie), `mustNotContainControl`, `trackEnMaxBytes`.
4. Niezmienniki dla KAZDEGO pliku (takze bledow): brak wyjatkow; wszystkie offsety
   w wynikach `<= fileSize`; `dataOffset <= dataEnd <= fileSize`; funkcje deterministyczne
   (dwa wywolania daja ten sam wynik).

### 2.2 Przypadki szczegolowe (jedna sekcja `SUBCASE` na zagrozenie)

| zagrozenie | pliki | co sprawdzic |
|---|---|---|
| H1, ADR 8.3-8.6 | `hdr_*` | kody wg manifestu; zawijanie UINT32 (`*_overflow_u32`) nie daje akceptacji; `Truncated`/`BadHeader` bez odczytu poza buforem (ASan) |
| H5, ADR 8.1-8.2 | `gz_*` | limit wyjscia zatrzymuje rozpakowanie (pomiar: szczytowa alokacja <= limit + epsilon, np. przez licznik alokatora testowego); ISIZE ignorowane; brak `Z_STREAM_END` = `DecompressionFailed`; gzip w gzip = `BadSignature` |
| H4, ADR 8.12 | `gd3_*` | GD3 uszkodzony nie psuje ladowania (loader: ok + `Gd3Invalid`); sanityzacja; przyciecie do `maxTagFieldBytes` (dokladnie 4096 B, bez rozcinania sekwencji UTF-8) |
| H3, ADR 8.9-8.10 | `blk_*` | kody wg manifestu; przy `blk_rom_budget_sum` kazdy blok osobno miesci sie w limicie, wiec test wykrywa dopiero SUME; ostrzezenia `BlockAfterLoop`/`BlockAfterSetData` |
| H2, ADR 8.8 | `stream_*` | ostatnia komenda ucieta -> `BadCommandStream`; `EndsEarly`; `UnknownCommand`; `ChipNotInHeader`; `stream_delay_overflow.vgz` -> `LimitExceeded` (bez zawiniecia licznika w UINT32) |
| limity | `limitCases[]` | kazdy przypadek z `limits` nadpisanymi w `Limits` |

### 2.3 Loader (`loader_corpus_test.cpp`)

- `loadTrackFromMemory` na calym korpusie: `LoadedTrack::bytes->size() == header.fileSize + kParserPadding`,
  ostatnie `kParserPadding` bajtow to zera, `raw().size() == header.fileSize`.
- `warnings` loadera = suma header + stream + gd3 (+ `Gd3Invalid` przy bledzie GD3).
- `loadTrackFromFile`: sciezka do pliku z korpusu; nieistniejacy -> `NotFound`; katalog -> blad
  (nie wyjatek); plik > `maxInputBytes` -> `TooLarge` bez alokacji calosci.
- `CancelToken` ustawiony przed wywolaniem -> `Cancelled`.

## 3. Golden audio (narzedzie `vgmrender`)

`vgmrender` (do napisania wraz z silnikiem; CLI: `vgmrender in.vgm --rate 44100 --seconds N
--out out.raw|wav [--no-loop]`) renderuje bez urzadzenia audio, deterministycznie, w watku
glownym. Dla plikow z tagiem `golden` w manifescie:

1. Render calego utworu (bez petli: do konca + ogon; z petla: do 2. przejscia petli).
2. Porownanie z plikiem wzorcowym `tests/golden/<nazwa>.json` zawierajacym: liczbe ramek, hash
   SHA-256 PCM (int16, stereo) ORAZ metryki tolerancyjne: RMS calosci, RMS w oknach 100 ms
   (30 wartosci), szczyt.
3. Kryterium: hash PCM identyczny na tej samej kompilacji/CPU (test "scisly", domyslnie wylaczony
   w CI miedzy kompilatorami); w CI porownanie tolerancyjne: |RMS - RMS_ref| / RMS_ref <= 1%,
   szczyt w granicach +-1 dB, dlugosc rowna co do ramki.
4. Wzorce NIE sa tworzone recznie ani z cudzego odtwarzacza; generuje je `vgmrender --update-golden`
   po przegladzie czlowieka (odsluch, bo brzmienia syntetyczne nie byly odsluchane - patrz README
   korpusu). Zmiana wzorca wymaga uzasadnienia w commicie.
5. Dodatkowe asercje bez wzorca: dlugosc renderu == `stream.totalSamples` * (rate/44100) +-1 ramka;
   niezerowy RMS dla plikow z nutami; cisza (RMS ~ 0) po ostatniej nucie przed ogonem;
   brak NaN/Inf; brak klipowania (|x| < 1.0) przy domyslnej glosnosci.
6. `ym2612_dac_compressed_v151.vgm` - tylko test regresji (kolejnosc bitow w paczce to zalozenie).

## 4. Test seek

Dla plikow `golden` z petla i bez:

- Render sekwencyjny do T vs `seek(T)` + render dalej: dla ukladow bez stanu wrazliwego na
  odtwarzanie (SN76489, YM2612 bez LFO) roznica RMS <= 1% w oknie 200 ms po T; dokladnie
  zgodna pozycja (`getPosition() == T` +-1 ramka).
- Seek do: 0, ostatniej ramki, `totalSamples` (-> koniec), `totalSamples + 1` (przycieta),
  w srodek bloku PCM (`ym2612_dac_*`: DAC gra dalej poprawnie), poza petle i do petli.
- Seek wstecz po przejsciu petli; seek wielokrotny w krotkim czasie (stabilnosc).
- Koszt: seek do 80% utworu na pliku z PCM < 1 s (cel z ADR 0002, "rzad ms vs ~1 s dla
  PCM-owych"; wartosc do zmierzenia, nie asercja twarda w CI).
- Petla: pozycja po `loopSamples` wraca do `loopStartSample`; `hasLoop()==false` dla
  `sn76489_loop_zero_samples_v150.vgm` (brak petli, utwor konczy sie).

## 5. Fuzzing (preset `clang-cl-asan-fuzz`)

Cele w `fuzz/` (libFuzzer + ASan), budowane `cmake --preset clang-cl-asan-fuzz`:

1. `fuzz_vgm_header` - bajty -> `decompressIfGzip` -> `parseVgmHeader` -> `parseGd3` ->
   `validateCommandStream`. Niezmienniki jak w 2.1(4). Nie wolno: wyjatku, UB, alokacji
   > `Limits` (ustaw male limity: 1 MiB, by fuzzer nie zjadal pamieci).
2. `fuzz_loader` - bajty -> `loadTrackFromMemory` (te same limity).
3. (po silniku) `fuzz_decode` - bufory, ktore przeszly walidacje -> `LoadFile + Start + Render(N)`
   w ASan (cel ADR 0002, "Fuzzing" pkt 2).

Korpus poczatkowy: wszystkie pliki z `tests/data/valid` i `malformed` (skopiowane do
katalogu seed). Slownik `fuzz/vgm.dict`: `"Vgm "`, `"Gd3 "`, bajty `\x66`, `\x67\x66`,
`\x1f\x8b\x08`, `\x90`..`\x95`.

Kryteria: 10 min bez crasha na CI (nightly: 1 h); crashe odtwarzane z zapisanych artefaktow i
dodawane jako regresje do `tests/data/malformed` (generator: nowy wpis w `build_corpus.py`).
Dla zagrozen H1-H3 test regresyjny na NIEprzefiltrowanym wejsciu (bezposrednio do libvgm, tylko w
buildzie ASan z libvgm) ma padac, a z walidatorem przechodzic (ADR 0002, "Fuzzing").

## 6. Kryteria przejscia E1 (testy)

1. Wszystkie przypadki z sekcji 2 zielone na `msvc-x64-debug`, `msvc-x64-release` i
   `clang-cl-asan-fuzz` (ASan: 0 raportow); zadnego wpisu `valid/` z ostrzezeniem.
2. 100% wpisow manifestu (81 plikow + limitCases) jest obslozonych przez test danych
   (test liczy pliki w katalogach i porownuje z manifestem - brak plikow-sierot).
3. Kazdy kod `ErrorCode`/`WarningCode` wymieniony w manifescie jest zaobserwowany w co najmniej
   jednym tescie (macierz pokrycia drukowana w tescie).
4. Golden: tolerancyjne porownanie zielone dla wszystkich plikow `golden`; hash scisly zgodny
   lokalnie na maszynie referencyjnej.
5. Seek: wszystkie przypadki z sekcji 4 zielone.
6. Fuzz: kryterium z sekcji 5 spelnione, brak otwartych crashy.
7. Zero alternatyw uzytych "po cichu": raport z testu wymienia wszystkie wpisy, w ktorych zaszedl
   wariant alternatywny (lista do przegladu przy zamykaniu kontraktu).

## 7. Znane luki korpusu

- Nie pokryto: `TooManyChipInstances` (naglowek VGM nie moze zadeklarowac > 2 instancji typu -
  wymaga innego mechanizmu), `HeaderLengthMismatch` (brak definicji w kontrakcie),
  `UnsupportedVersion` powyzej zakresu, wlasciwego `OutOfMemory`, `Cancelled` (test w kodzie),
  rdzenie ROM inne niz okim6295/c352 (H3), zapisy RAM 0xC0+ w wersji poprawnej.
- Zapisy `ym2612`/`ym2151` itd. nie sa walidowane semantycznie (parser ich nie interpretuje).
- Brzmienia i strojenie nut nie byly odsluchane.
