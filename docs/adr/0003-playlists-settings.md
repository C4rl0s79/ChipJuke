# ADR 0003: Playlisty, biblioteka i ustawienia

- Status: wdrozone (E2)
- Data: 2026-11-24
- Dotyczy: `src/playlist`, `src/library`, `src/settings`, kontrakty `include/vgmplay/{playlist,library,settings}.hpp`
- Decyzje uzytkownika: wiele nazwanych playlist + import/eksport M3U/M3U8; tylko wersja przenosna (ZIP);
  ustawienia obok `.exe`, gdy folder zapisywalny, inaczej `%APPDATA%\vgmplay`; jezyk PL/EN; tytuly GD3
  domyslnie EN; pad nie dziala w tle (stala); 2 przejscia petli, wyciszenie 1 s, utwory bez petli bez
  wyciszania; dlugosc wiersza wzorca 1/20 s (`rowTicks` 2205).

## Pliki i lokalizacje

| Plik | Gdzie | Zawartosc |
|---|---|---|
| `settings.json` | obok `.exe` (tryb przenosny) albo `%APPDATA%\vgmplay\` | `Settings`, `schemaVersion` 1 |
| `playlists.json` | katalog ustawien (wybiera aplikacja) | `PlaylistModel`, `schemaVersion` 1 |
| `library-cache.json` | katalog ustawien | `MetaCache` (czas, tagi GD3), `schemaVersion` 1 |
| `*.bak` / `*.bad` | obok uszkodzonego pliku | kopia uszkodzonego oryginalu |

`resolveSettingsLocation(exeDir, appDataDir)`: jesli `isDirectoryWritable(exeDir)` (proba utworzenia pliku
`.vgmplay-write-test-*.tmp` z `FILE_FLAG_DELETE_ON_CLOSE`, katalogu nie tworzy) - `exeDir\settings.json`,
`portable = true`; w przeciwnym razie `appDataDir\vgmplay\settings.json`. `appDataDir` jest wstrzykiwany
(testy nie dotykaja prawdziwego `%APPDATA%`). Aplikacja powinna trzymac `playlists.json` i
`library-cache.json` w tym samym katalogu co `settings.json`.

## Format JSON

Wszystkie pliki: UTF-8 bez BOM, deterministyczne (klucze alfabetycznie, wciecie, koniec linii `\n`), sciezki z `/`
jako separatorem, enumy jako nazwy tekstowe (nie liczby; wartosci enumow nie sa trwale).

- Ustawienia: sekcje `audio`, `playback` (`nonLoopEnd`: `stop`/`fadeOut`; `cores`: `type` = `chipTypeName`),
  `ui` (`language` `pl`/`en`, `gd3Language` `en`/`jp`, `rowTicks`), `input` (`bindings`: `action`, `source`, `code`),
  `session` (`trackPath`, `positionMs`). Nieznane pola ignorowane; brakujace = domyslne; zly typ = domyslne +
  `Warning`; zakresy przycinane z `Warning` (bufferMs 10..500, sampleRate 0 lub 8000..192000, volume 0..1,
  loopCount 0..99, fadeMs 0..30000, fontScale 0.5..3, rowTicks 64..44100, repeatDelayMs 100..2000,
  repeatRateMs 10..1000). Liczby zmiennoprzecinkowe zaokraglane do 3 miejsc przy zapisie.
- Playlisty: `{schemaVersion, active, playlists:[{name, cursor, entries:[{path, title?, durationMs?}]}]}`.
  Limity: 1000 playlist, 100000 wpisow na liste (`LimitExceeded`). Nazwy unikalne bez rozrozniania wielkosci liter
  (Unicode), <= 256 B. Zly kursor/`active` -> bezpieczne wartosci (-1 / 0). Wpisy-URL i sciezki urzadzen
  w JSON sa pomijane (UNC dozwolone: to dane zapisane przez aplikacje).
- Cache metadanych: klucz = sciezka (bez rozrozniania wielkosci liter) + rozmiar + `mtime` (FILETIME).
  Niepelne wpisy pomijane; limit 200000 wpisow.

## Migracje i wersje

`schemaVersion` nowszy niz znany -> `UnsupportedSchema` (plik zostaje nietkniety, powstaje kopia). Starszy ->
migracja krokowa w gore (`migrate()` w `settings.cpp`); brak pola = wersja 0, ktora ma uklad v1 (przepisanie
z `Warning`). Nowa wersja = nowy `case` w `migrate()` + test.

## Zapis atomowy i odpornosc na uszkodzenia

Zapis: plik tymczasowy `<plik>.tmp<pid>_<n>` w TYM SAMYM katalogu, `FlushFileBuffers`, potem `ReplaceFileW`
(cel istnieje) albo `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`; ponowienia przy chwilowym
`ACCESS_DENIED`/`SHARING_VIOLATION` (antywirus); przy bledzie plik tymczasowy jest usuwany. Brakujace katalogi
sa tworzone. Wspolny kod: `src/settings/fs_util.hpp` (wewnetrzny, bez kontraktu).

Odczyt: ograniczony rozmiarem (ustawienia 4 MiB, playlisty 256 MiB), rozmiar sprawdzany na uchwycie.
Brak pliku nie jest bledem (`Settings` domyslne / pusty `PlaylistModel` / pusty cache). Plik uszkodzony
(skladnia, obciety, nowszy schemat): zwracany blad, oryginal NIE jest modyfikowany, obok powstaje kopia
(`playlists.json.bak`, `settings.json.bad`). Aplikacja nie powinna nadpisywac pliku bez wiedzy uzytkownika po takim
bledzie; po zapisie domyslnych danych kopia `.bak`/`.bad` zostaje.

## Bezpieczenstwo sciezek (M3U/M3U8 i skan)

`importM3u` odrzuca pozycje (z `Warning::PathRejected`, licznik `rejected`; reszta pliku jest wczytywana):

1. URL: dowolny `schemat://` (takze `C://x`), oraz kazdy dwukropek poza litera dysku na pozycji 1
   (`mailto:x`, strumienie NTFS `a.vgm:s`, `C:wzgledne`).
2. UNC i sciezki urzadzen: wszystko zaczynajace sie od `\\` lub `//` (`\\serwer\udzial`, `\\?\`, `\\.\`).
3. Znaki sterujace, `* ? " < > |`; skladniki konczace sie kropka lub spacja; zarezerwowane nazwy urzadzen
   (`CON`, `NUL`, `COM1`..`LPT9`, takze z rozszerzeniem).
4. Sciezki absolutne (z dyskiem lub zaczynajace sie od `\`) - tylko przy `allowAbsolutePaths`.
5. Wyjscie poza `allowedRoot` (pusty = `baseDir`): normalizacja leksykalna (`..`, `.`, podwojne separatory),
   porownanie skladnik po skladniku bez rozrozniania wielkosci liter; nastepnie `weaklyResolve` - istniejacy
   przodek jest rozwiazywany przez system (junction/symlink), wynik musi nadal lezec w rozwiazanym `allowedRoot`.
   Gdy rozwiazanie jest niemozliwe, a w sciezce sa dowiazania, pozycja jest odrzucana (bezpieczniej).
   Wyjatek: wolumin bez `GetFinalPathNameByHandle` i bez dowiazan -> wynik leksykalny.
   `std::filesystem::weakly_canonical` nie jest uzywane (zawodzi w MSVC STL na dyskach RAM/subst).
6. Istnienie pliku nie jest sprawdzane. Zwrocone sciezki sa znormalizowane leksykalnie (bez `..`).
   Ograniczenie: kontrola w chwili importu (TOCTOU); loader i tak otwiera tylko zwykle pliki.

Kodowanie: BOM UTF-8 wygrywa; bez BOM `assumeUtf8` (true = UTF-8, niepoprawna linia odrzucona; false = strona kodowa
systemu przez `MultiByteToWideChar(CP_ACP)`); UTF-16 nieobslugiwany (`ParseError`). Linie rozdzielane `CRLF`, `LF`
lub samym `CR`. `#EXTINF:<sekundy>,<tytul>` dotyczy tylko nastepnej pozycji; inne komentarze ignorowane.
Eksport: `CRLF`, sciezki wzgledne do `baseDir` gdy leza w nim, inaczej absolutne z `/`; `utf8=false` zamienia
znaki spoza strony kodowej na `?` (stratne - zalecany M3U8).

`ScanJob`: nie podaza za junction i dowiazaniami symbolicznymi (`ReparsePointSkipped`; inne znaczniki, np. OneDrive,
traktowane jak zwykle pliki), root moze sam byc dowiazaniem (wybor uzytkownika); `maxDepth`, `maxFiles`,
anulowanie sprawdzane przy kazdej pozycji katalogu; bledy dostepu to `Warning`, nie wyjatki; komunikaty zawieraja
tylko sciezki wzgledne do roota; przy `followReparsePoints=true` cykle wykrywane po rozwiazanym celu.

## Dodatki do kontraktow (addytywne)

- `Settings::ui.rowTicks` (2205), `Settings::session {trackPath, positionMs}`.
- `fromJson(json, std::vector<Warning>* = nullptr)`, `loadSettings(file, std::vector<Warning>* = nullptr)`.
- `library`: `FileStamp`/`statFile`, `TrackMeta`/`readTrackMeta` (przez `loader::loadTrackFromFile` - jedyne zrodlo
  prawdziwej dlugosci z pre-skanu; naglowek bywa falszywy), `MetaCache`, `loadMetaCacheFile`/`saveMetaCacheFile`.
  Kontrakt `ScanJob` (bez parsowania zawartosci) zostal bez zmian.

## Sesja

Aktywna playlista i kursor zapisuje `playlists.json` (`active`, `cursor` na liscie). Pozycja w utworze:
`Settings::session.positionMs` razem z `trackPath`; UI przywraca pozycje tylko gdy sciezka biezacego wpisu
kursora jest rowna `trackPath`.
