# Korpus testowy VGM/VGZ

Pliki w `valid/` i `malformed/` oraz `manifest.json` sa GENEROWANE przez `tools/vgmgen/`.
Nie edytuj ich recznie. Zadne pliki nie pochodza z rippow (vgmrips itp.) - melodie sa wlasne lub
z domeny publicznej ("Oda do radosci", gama C-dur), patche to proste ustawienia rejestrow.

## Regeneracja

Z korzenia repozytorium (Python 3.10+, tylko biblioteka standardowa):

```
python -I tools/vgmgen/build_corpus.py
```

Opcje: `--out KATALOG` (domyslnie `tests/data`), `--allow-missing-names` (patrz nizej).
Skrypt konczy sie kodem != 0, gdy: nazwa bledu/ostrzezenia/ukladu w manifescie nie istnieje w
`include/vgmplay/{core,chips}.hpp` (kod 3), korpus >= 1 MiB (kod 4).
Niezalezny skaner strumienia: `python -I tools/vgmgen/vgmgen.py scan PLIK`.

### Determinizm

Dwa uruchomienia daja identyczne bajty (SHA-256 wszystkich plikow). Gzip jest budowany recznie:
naglowek z `mtime=0`, `OS=255`, deflate z zlib (poziom 9). Gwarancja dotyczy tej samej wersji zlib;
inna wersja/implementacja zlib (np. zlib-ng) moze dac inne bajty `.vgz` przy tej samej tresci.
Dlatego testy NIE powinny porownywac hashy plikow `.vgz` - manifest zawiera `rawSha256`
(hash tresci po rozpakowaniu), ktory jest niezalezny od zlib.

## Struktura

- `valid/` - 18 poprawnych plikow (wersje 1.00 .. 1.71, petla/bez, GD3/bez, pola JP, .vgz,
  dual-chip, bloki PCM zwykle i skompresowane, DAC stream 0x90-0x95, naglowek rozszerzony).
  Tag `golden` w manifescie = plik nadaje sie do testow golden audio.
- `malformed/` - pliki uszkodzone lub graniczne, po jednym zagrozeniu na plik (H1-H6 z
  `docs/research/libvgm-parser-hazards.md`, lista kontroli z ADR 0002 pkt 8). Czesc z nich to
  poprawne pliki z OSTRZEZENIEM (`expect.result == "ok"`).
- `manifest.json` - oczekiwania (nizej).

## Format manifestu (schemaVersion 1)

`files[]`:

| pole | znaczenie |
|---|---|
| `path` | sciezka wzgledem `tests/data` (`valid/...`, `malformed/...`) |
| `kind`, `description`, `hazard`, `tags`, `note` | opis; `hazard` = odwolanie do H1-H6/ADR |
| `sizeBytes`, `sha256` | plik na dysku; dla `.vgz` dodatkowo `rawSizeBytes`, `rawSha256` |
| `limits` | (opcjonalne) pola `format::Limits` do ustawienia na czas testu |
| `expect.result` | `"ok"` albo `"error"` |
| `expect.error` | nazwa `ErrorCode` (gdy `result == "error"`) |
| `expect.warnings` | nazwy `WarningCode`, ktore MUSZA wystapic (zbior; kolejnosc bez znaczenia) |
| `expect.alternatives[]` | inne dopuszczalne wyniki, gdy kontrakt jest niejednoznaczny (patrz nizej) |
| `header` | (dla `ok`) oczekiwane `HeaderInfo`: `versionBcd`, `totalSamples` (z NAGLOWKA), `loopSamples`, `hasLoop`, `rate`, `dataOffset`, `dataEnd`, `gd3Offset`, `fileSize` (bajty po dekompresji), `chips[]` = `{type, instance, clockHz[, volume]}` |
| `stream` | (dla `ok`) oczekiwane `StreamReport`: `totalSamples` (PRAWDZIWA dlugosc = suma opoznien), `loopStartSample`, `commandCount`, `pcmBytes`, `romWriteBytes`, `hasEndCommand` |
| `gd3` | oczekiwane pola `Gd3Tag` (`trackEn`, `trackJp`, ... `releaseDate`, `converter`, `notes`) albo `{"present": false}`; dla plikow ze znakami do sanityzacji: `trackEnContains`, `mustBeValidUtf8`, `trackEnMaxBytes` itp. |

`limitCases[]`: ten sam plik z innymi `Limits` (`id`, `path`, `limits`, `expect`) - dla limitow,
ktorych nie da sie wywolac malym plikiem w repo (np. 64 MiB).

`unverifiedNames` (obecne tylko, gdy niepuste): nazwy uzyte w manifescie, ktorych nie ma w kontrakcie.

### Semantyka oczekiwan

- `expect` opisuje wynik PODSTAWOWY. Jesli jest `alternatives`, test powinien zaakceptowac
  podstawowy ALBO ktorykolwiek z alternatywnych wynikow (kontrakt nie rozstrzyga). Test ma wtedy
  wypisac, ktory wariant zaszedl, aby niejednoznacznosci dalo sie pozniej zamknac.
- `commandCount` liczy KAZDA komende wlacznie z 0x66 i z blokiem 0x67 jako jedna komenda -
  to zalozenie generatora; kontrakt tego nie precyzuje.
- `pcmBytes`: bloki 0x00..0x3F = dlugosc danych, 0x40..0x7E = `decmpLen` (po dekompresji), 0x7F
  nie liczy sie. `romWriteBytes`: suma `memSize` blokow 0x80..0xBF. To zalozenie generatora.
- `header.chips[]`: `clockHz` bez bitow flag (bit 30/31). `instance` 1 = bit 30. Kolejnosc
  w tablicy nie jest gwarantowana - porownuj po `(type, instance)`.
- Dla plikow, ktorych oczekiwanie to blad, brak sekcji `header`/`stream`.

## Zalozenia generatora niezweryfikowane z oryginalna specyfikacja VGM

- Struktura naglowka rozszerzonego v1.70 (`sn76489_extra_header_v170.vgm`) i ID ukladu w nim.
- Kolejnosc bitow w paczce bloku skompresowanego (MSB first) i uklad naglowka 0x40 (10 B)
  oraz 0x7F (6 B + tabela) - zgodne z ADR 0002 pkt 9, ale nie sprawdzone z kodem libvgm.
- Brzmienia ukladow nie byly odsluchane; wzory na f-num (zwlaszcza YM2413) moga dawac przestrojone
  nuty. Testy golden maja traktowac wynik jako regresje, nie jako wzorzec poprawnosci.
- `stream_unknown_cmd_unskippable.vgm` uzywa komendy 0x69 przy zalozeniu, ze nie ma ona
  zdefiniowanej dlugosci.
