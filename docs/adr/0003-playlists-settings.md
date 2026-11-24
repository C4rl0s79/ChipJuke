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

Zapis: plik tymczasowy `<plik>.tmp<32 znaki hex>` (128 bitow z `BCryptGenRandom`) w TYM SAMYM katalogu, otwierany
`CREATE_NEW` + `FILE_FLAG_OPEN_REPARSE_POINT` i po otwarciu sprawdzany jako zwykly plik dyskowy (nie dowiazanie,
nie katalog); `FlushFileBuffers`, potem `ReplaceFileW` (cel istnieje) albo `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`;
ponowienia przy chwilowym `ACCESS_DENIED`/`SHARING_VIOLATION` (antywirus); przy bledzie plik tymczasowy jest usuwany.
Na poczatku zapisu sprzatane sa osierocone pliki `<plik>.tmp<wzorzec>` (32 hex albo stary `<pid>_<n>`) starsze niz
1 godzina; obce nazwy i swieze pliki nie sa ruszane. Brakujace katalogi sa tworzone. Wspolny kod:
`src/settings/fs_util.hpp` (wewnetrzny, bez kontraktu).

Odczyt: ograniczony rozmiarem (ustawienia 4 MiB, playlisty i cache 64 MiB), rozmiar sprawdzany na uchwycie.
Parser JSON (`json_limits.hpp`, SAX nad standardowym budowniczym DOM) liczy zagniezdzenie (max 8 poziomow) i
liczbe wartosci (ustawienia 50000, playlisty i cache 4 mln) PODCZAS parsowania: `LimitExceeded` bez budowy calego
DOM i bez rekursji (test: 100000 x `[`). Wariant z `parser_callback_t` odrzucono: przy 200000 wpisow cache
wczytywal sie 62 s (zlozonosc kwadratowa); SAX: 0,57 s (zapis 0,45 s, release, 31 MiB JSON).
Brak pliku nie jest bledem (`Settings` domyslne / pusty `PlaylistModel` / pusty cache). Plik uszkodzony
(skladnia, obciety, nowszy schemat): zwracany blad, oryginal NIE jest modyfikowany, obok powstaje kopia
(`playlists.json.bak`, `settings.json.bad`): poprzednia kopia (takze dowiazanie podstawione w jej miejsce) jest
usuwana, potem `CopyFileW(bFailIfExists=TRUE)`, wiec kopia nigdy nie pisze przez dowiazanie. Aplikacja nie
powinna nadpisywac pliku bez wiedzy uzytkownika po takim bledzie; po zapisie domyslnych danych kopia zostaje.

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

`ScanJob`: nie podaza za junction i dowiazaniami symbolicznymi (`ReparsePointSkipped`; tylko znaczniki SYMLINK i
MOUNT_POINT - inne znaczniki reparse, np. dedup, traktowane jak zwykle pliki), root moze sam byc dowiazaniem (wybor
uzytkownika); `maxDepth`, `maxFiles`, anulowanie sprawdzane przy kazdej pozycji katalogu; stos katalogow do
odwiedzenia ograniczony do 100000 (`LimitReached`); pozycje o nazwie zarezerwowanej (CON, NUL, COM1...) lub z
koncowa kropka/spacja sa pomijane (`EntrySkipped`; ten sam predykat `isUnsafeComponentName` co w M3U); bledy dostepu
to `Warning`, nie wyjatki; komunikaty zawieraja tylko sciezki wzgledne do roota; przy `followReparsePoints=true`
cykle wykrywane po rozwiazanym celu.

Pliki w chmurze (OneDrive / Cloud Files / HSM): `FoundFile::attributes` niesie `dwFileAttributes`. Pliki z
`FILE_ATTRIBUTE_OFFLINE`, `RECALL_ON_OPEN` lub `RECALL_ON_DATA_ACCESS` (`isCloudPlaceholder`) sa na liscie skanu
(z Warning "cloud placeholder"), ale `readTrackMeta`/`MetaCache::getOrRead` ich NIE czytaja (odczyt wymusilby
pobranie z sieci): zwracaja `AccessDenied` rozpoznawany przez `isCloudPlaceholderError`. Parametr
`allowCloudDownload=true` ("pobierz mimo to") czyta plik. Trafienie w cache nie czyta pliku. Propozycja dla core.hpp:
`ErrorCode::CloudPlaceholder` i `WarningCode::CloudPlaceholderSkipped` (dzis: `AccessDenied`/`Other`).

Sciezki w plikach JSON (playlisty, cache, `session.trackPath`): wspolny predykat `isUnsafeStoredPath`. Zawsze
odrzucane: URL (`schemat://`), przestrzen urzadzen (prefiksy `//?/` i `//./` w obu wariantach separatora), potoki
(`//host/pipe/...`). Sciezki UNC (`//serwer/udzial`) domyslnie tez (`allowNetworkPaths=false`; opcja w
`PlaylistLoadOptions`, `fromJson`/`loadSettings`, `MetaCache::fromJson`/`loadMetaCacheFile`).
`resolveSettingsLocation` ignoruje `appDataDir` zaczynajacy sie od dwoch separatorow. Loader
(`isDeviceNamespacePath`) odrzuca dodatkowo prefiks `Global`, `UNC/<host>/pipe/` w formie rozszerzonej oraz
`//<host>/pipe/` po nazwie, przed jakimkolwiek otwarciem.

Teksty z zewnatrz (tytuly `#EXTINF`, JSON, cache): `sanitizeTitle` usuwa znaki < 0x20 (TAB -> spacja), DEL, C1,
znaki kierunku i niewidoczne (U+200B-200F, U+202A-202E, U+2066-2069), niepoprawny UTF-8 -> U+FFFD; limit 512 B na
granicy znaku. `durationMs`/`loopMs` z JSON i cache przycinane do 24 h. `importM3u`: wejscie > 16 MiB -> `TooLarge`,
po 1 000 000 linii import sie konczy (`LimitReached`).

`isDirectoryWritable` otwiera plik probny z `GENERIC_WRITE | DELETE` (DELETE jest wymagane przez
`FILE_FLAG_DELETE_ON_CLOSE`); test wielokrotnej proby na NTFS nie wykazal falszywych "niezapisywalne".

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

Dodatki E2-sec (wszystkie addytywne, parametry domyslne): `PlaylistLoadOptions` + drugi parametr `loadJson` i
`loadPlaylistsFile`; trzeci parametr `allowNetworkPaths` w `fromJson` i `loadSettings`; `FoundFile::attributes`,
`kAttr*`, `isCloudPlaceholder`; `readTrackMeta(path, cancel, allowCloudDownload)`, `isCloudPlaceholderError`,
`MetaCache::getOrRead(..., allowCloudDownload)`, `MetaCache::fromJson(json, allowNetworkPaths)`,
`loadMetaCacheFile(file, allowNetworkPaths)`. `MetaCache` trzyma wpisy w `unordered_map` (zapis sortuje klucze).
