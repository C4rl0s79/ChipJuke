# ChipJuke — instrukcja użytkownika (PL)

> ChipJuke to niezależny projekt. Emulację układów zapewnia biblioteka libvgm (Valley Bell), wbudowana statycznie
> w `ChipJuke.exe`. Wcześniejsza robocza nazwa to *vgmplay*: ustawienia z `%APPDATA%\vgmplay` są przy pierwszym
> uruchomieniu przenoszone do `%APPDATA%\ChipJuke`.

ChipJuke to odtwarzacz plików VGM / VGZ w stylu trackera dla Windows. Wersja 1.0.0-rc2 (kandydat na wydanie).
Licencja: GPL-2.0-only. Angielski opis projektu: [README.md](../README.md).

## Wymagania

- Windows 10 22H2 lub nowszy, 64-bitowy.
- Karta graficzna ze sterownikiem Direct3D 11 oraz urządzenie wyjściowe audio.
- Nic nie trzeba instalować: jeden plik `ChipJuke.exe`, bez dodatkowych DLL i bez pakietu Visual C++ Redistributable.

## Instalacja (wersja przenośna)

1. Pobierz `ChipJuke-<wersja>-win64.zip`.
2. Opcjonalnie sprawdź sumę: `Get-FileHash ChipJuke-<wersja>-win64.zip -Algorithm SHA256` (PowerShell) — wynik musi
   być taki jak w `SHA256SUMS.txt`.
3. Rozpakuj ZIP w dowolnym miejscu (np. `D:\Narzedzia\ChipJuke`) i uruchom `ChipJuke.exe`.

Nie ma instalatora. Odinstalowanie to usunięcie folderu (oraz `%APPDATA%\ChipJuke`, jeśli powstał).
W folderze `samples\` są krótkie, syntetyczne pliki testowe z generatora projektu (to nie są rippy z gier).

## Uruchamianie

```
ChipJuke.exe [opcje] [plik.vgm|plik.vgz|folder ...]
```

- bez argumentów: wraca ostatnia sesja (playlisty, utwór, pozycja) — w pauzie;
- plik: zostaje dodany do aktywnej playlisty i od razu gra;
- folder: wszystkie pliki z folderu są dodawane (skan w tle);
- pliki i foldery można też przeciągnąć na okno albo dodać przez Ctrl+O / Ctrl+Shift+O.

| Opcja | Znaczenie |
|---|---|
| `--pl` | start z interfejsem po polsku (F2 przełącza PL/EN w trakcie) |
| `--settings-dir=<katalog>` | własny katalog ustawień, playlist i pamięci metadanych |
| `--allow-network-paths` | dopuszcza ścieżki sieciowe `\\serwer\...` w playlistach (domyślnie odrzucane) |
| `--no-autoplay` | po załadowaniu nie startuje odtwarzania |
| `--null-audio` | bez dźwięku (testy / CI) |
| `--diag` | start z nakładką diagnostyczną (jak F12) |
| `--row-ticks=N` | długość wiersza wzorca w tickach 44100 Hz (735 = 1/60 s) |
| `--scopes-large` / `--scopes-hidden` | startowa wysokość paska oscyloskopów |

Opcje `--fake-channel-scopes` i `--fake-pattern` służą tylko do diagnostyki.

## Gdzie zapisywane są ustawienia

Pliki `settings.json`, `playlists.json` i `library-cache.json` leżą **obok `ChipJuke.exe`**, jeśli ten folder
jest zapisywalny (tryb przenośny; nie trzeba żadnego pliku-znacznika). W przeciwnym razie (np. rozpakowanie do
`C:\Program Files`) trafiają do `%APPDATA%\ChipJuke`. Flaga `--settings-dir=<katalog>` ma pierwszeństwo.
Uszkodzony plik ustawień jest zachowywany jako kopia `.bad`, a program startuje z ustawieniami domyślnymi.

## Okno programu

Cztery panele: **Transport** (przyciski, pozycja, głośność), **Wzorzec** (oscyloskopy kanałów, VU i siatka nut),
**Playlista** i **Info** (GD3 i dane pliku). Między panelami przełączasz Tab / Shift+Tab (pad: RB / LB).

Wzorzec pokazuje prawdziwe nuty, instrumenty i głośności odczytane z rejestrów układów dźwiękowych. Nuty są
dekodowane dla: SN76489, YM2612 (+DAC), YM2151, YM2203, YM2608 (OPNA), YM2610/B (OPNB), YM2413, YM3812/YM3526, Y8950, YMF262, AY8910, Konami SCC (K051649), NES APU/FDS,
OKIM6295, OKIM6258, C140, K054539, K053260, SegaPCM, QSound, GA20. Dla układów PCM nuta jest względna (ta sama próbka
i tempo = ta sama nuta). Pozostałe układy mają kolumnę „...” z podpisem n/a, a dźwięk gra normalnie.

OPL4 (YMF278B, Moonsound) potrzebuje ROM-u próbek Yamahy `yrw801.rom` dla wbudowanych instrumentów. Program go nie
zawiera — połóż własną kopię obok `ChipJuke.exe` albo obok utworu. Bez niego panel Info pokazuje ostrzeżenie, a
instrumenty z ROM-u są ciche.

## Sterowanie — klawiatura

Wszystkie przypisania można zmienić w Ustawieniach (F10 → Przypisania...).

| Klawisz | Działanie |
|---|---|
| Spacja | play / pauza |
| Esc | stop (albo zamknięcie komunikatu / cofnięcie) |
| P / N | poprzedni / następny utwór |
| Tab / Shift+Tab | następny / poprzedni panel |
| Lewo / Prawo (Transport) | przewijanie ±5 s |
| Góra / Dół (Transport) | głośność |
| + / − | krótszy / dłuższy wiersz wzorca (1/120 … 1/10 s) |
| Lewo / Prawo (Wzorzec) | wybór kanału |
| M lub Enter, S | wyciszenie / solo wybranego kanału |
| 1…9, 0 oraz Shift+1…0 | wyciszenie / solo kanałów 1…10 |
| U / C / R | włącz wszystkie / wyczyść solo / reset obu |
| Góra / Dół, Enter, Delete (Playlista) | wybór, odtwarzanie, usunięcie wpisu (Ctrl+Góra/Dół przenosi) |
| Lewo / Prawo (Playlista) | inna playlista |
| Ctrl+O / Ctrl+Shift+O | dodaj pliki / dodaj folder |
| Ctrl+I / Ctrl+E | import / eksport M3U |
| F2 | język interfejsu PL/EN |
| F3 | pola GD3 angielskie / japońskie |
| F4 | pasek oscyloskopów: mały / duży / ukryty |
| F6 / F7 | powtarzanie Off/All/One / losowanie |
| F8 / F9 / Shift+Delete | nowa / zmień nazwę / usuń playlistę |
| F10 | Ustawienia |
| F12 | diagnostyka (FPS, WASAPI, bufor, underruny) |

Mysz: klik w pasek pozycji przewija, suwak ustawia głośność, kółko przewija panele, dwuklik w playliście odtwarza
wpis, prawy klik otwiera menu wpisu, przeciągnięcie plików lub folderów na okno dodaje je do playlisty.

## Sterowanie — pad

Układ Xbox (SDL mapuje inne pady na te same pozycje). Pad działa tylko, gdy okno ma fokus. Cały program da się
obsłużyć padem; wyraźna ramka pokazuje fokus, a pasek podpowiedzi na dole (wyłączany w Ustawieniach) — przyciski.

| Pad | Działanie |
|---|---|
| D-pad / lewy drążek | nawigacja (z autopowtarzaniem) |
| A / B | potwierdź / wstecz (B zawsze cofa) |
| X | solo (Wzorzec) / usuń wpis (Playlista, z potwierdzeniem) |
| Y | menu wpisu / ustawienia |
| Start | play / pauza |
| LB / RB | poprzedni / następny panel |
| LT / RT albo L3 / R3 | poprzedni / następny utwór |
| prawy drążek góra/dół | strona w górę / w dół |
| Select | wysokość paska oscyloskopów |

Pozycje otwierające okna systemowe (dodawanie plików lub folderu, import/eksport M3U) wymagają klawiatury lub myszy.

## Ustawienia (F10)

Pętle, wyciszenie na końcu, zachowanie utworów bez pętli, głośność, język interfejsu, język GD3, długość wiersza
wzorca, powtarzanie, wysokość paska oscyloskopów, **urządzenie audio** (Lewo/Prawo wybiera, A odświeża listę;
gdy urządzenia brak — używane jest domyślne z komunikatem), **ścieżki sieciowe** (włączenie z potwierdzeniem),
podpowiedzi przycisków oraz **Przypisania...** (zmiana klawiszy i przycisków pada dla każdej akcji: A — „naciśnij
przycisk” przez 6 s, Esc lub przytrzymane B przez 1 s anuluje, konflikt → ostrzeżenie i zamiana, X/Del czyści,
„Przywróć domyślne”). Ustawienia zapamiętują się między uruchomieniami.

## Ścieżki sieciowe

Domyślnie ścieżki sieciowe (`\\serwer\udział\...`) są wyłączone (opcja `--allow-network-paths` albo przełącznik
w Ustawieniach). Dostęp do serwera może wysłać do niego dane logowania Windows, dlatego program nie łączy się z siecią
bez zgody. Dotyczy to też wbudowanej przeglądarki plików:

- **dyski zmapowane na udział sieciowy** (`net use Z: \\serwer\udział`) są na liście dysków wyszarzone z podpisem
  „sieciowy - włącz ścieżki sieciowe w Ustawieniach” i nie da się do nich wejść (ani przez „ostatnie foldery”);
- **dowiązania symboliczne i junctions**, których cel to ścieżka sieciowa, są pokazane jako zablokowane; program nie
  otwiera ich celu (nie podąża za dowiązaniem) i nie wchodzi do takiego folderu. Wejście w dowiązanie rozwiązuje cel
  bez kontaktu z siecią; dowiązanie do pliku w sieci jest pomijane na liście;
- zmiana ustawienia działa od następnego otwarcia folderu, bez restartu.

Ograniczenie: zapisane ścieżki (playlisty, sesja) są sprawdzane tekstowo, więc litera dysku zmapowana na sieć albo
utworzona przez `subst` jest dla nich zwykłą ścieżką lokalną. Pliki podane z takiej litery (wiersz poleceń, przeciągnięcie
na okno) zostaną otwarte; blokada obejmuje przeglądarkę plików. Aby całkiem wyłączyć sieć, nie mapuj udziałów
na litery dysków używane z programem.

## Znane ograniczenia

- Układy bez dekodera nut pokazują kolumnę n/a (dźwięk nie jest dotknięty).
- „Brak nut” przy grającym kanale to zwykle wybrzmiewanie po zwolnieniu klawisza (długi release instrumentu) —
  wzorzec tego nie pokazuje.
- Przełączania urządzenia WASAPI i podłączania pada w trakcie pracy nie testowano automatycznie na prawdziwym
  sprzęcie. Gdy dźwięk nie otworzy się już przy starcie, program ponawia próbę dopiero po zmianie urządzenia.
- W wąskim oknie fokus pada może stanąć na ukrytym przycisku paska wzorca.
- W oknach usuwania domyślnie zaznaczone jest „Tak”.
- Otwieraj wyłącznie zaufane pliki VGM/VGZ. Parser jest utwardzony i fuzzowany, ale to wersja kandydująca.
- Część rdzeni libvgm ma niejasną licencję upstream (zob. `licenses\libvgm-audit.md`).

## Licencje i kod źródłowy

ChipJuke jest na licencji GPL-2.0-only (plik `LICENSE`). Składniki zewnętrzne: `THIRD_PARTY_NOTICES.md` i folder
`licenses\`. Kod źródłowy odpowiadający tej binarce: plik `SOURCE.txt` (adres repozytorium i commit).
