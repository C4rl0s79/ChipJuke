# Checklista testow recznych przed wydaniem (ChipJuke)

Wykonuje czlowiek na skompilowanym artefakcie wydania (ten sam plik `ChipJuke.exe`, ktory trafi do uzytkownikow),
NIE na buildzie deweloperskim. Testy automatyczne (doctest MSVC, kampania fuzzingu) sa warunkiem wstepnym,
nie zamiennikiem - ta lista obejmuje to, czego automat nie widzi: sprzet, system, DPI, chmure, siec, czas.

## Jak uzywac

- Skopiuj ten plik do `docs/testing/results/release-<wersja>.md` (albo wydrukuj) i wypelniaj pole **Wynik**.
- Wynik: `OK`, `BLAD` (+ numer zgloszenia), `N/D` (+ powod). Kazde `BLAD` blokuje wydanie, chyba ze
  swiadoma decyzja wlasciciela produktu jest wpisana w sekcji 0.
- Zapisuj **przy kazdym punkcie** to, co zaskoczylo, nawet jesli wynik `OK` (nazwa pliku, czas, komunikat).
- Zasada ogolna dla wszystkich punktow: aplikacja nie moze sie zawiesic, wywrocic (okno znika bez komunikatu),
  zuzywac stale >1 rdzenia w bezczynnosci ani zostawiac po zamknieciu procesu w Menedzerze zadan.

## 0. Dane wydania

| Pole | Wartosc |
|---|---|
| Wersja / tag | |
| Skrot SHA-256 `ChipJuke.exe` (`Get-FileHash`) | |
| Sposob dystrybucji (zip / instalator / sam exe) | |
| Tester, data | |
| Skrot commita buildu, preset (`msvc-x64-release`) | |
| Wynik automatow (doctest: liczba przypadkow / asercji; fuzz: data kampanii) | |
| Decyzje o znanych usterkach (jesli sa) | |

### Zasoby testowe

- Pliki reprezentatywne: `D:\emu\vgm` (tylko odczyt) oraz syntetyczne `tests\data\valid\*.vgm` (ukladow, ktorych nie
  ma w `D:\emu\vgm`: YMF262, wiele instancji "dual", itd.).
- Lista ukladow wykrytych w `D:\emu\vgm` (naglowki; do weryfikacji w punkcie 6): YM2151, YM2612, YM2203, YM2413,
  YM3812, SN76489, AY8910, NES APU, SegaPCM, OKIM6258, OKIM6295, C140, K053260, K054539. Naglowki z polami rozszerzonymi
  (ES5503, ES5506, GA20, SAA1099, WonderSwan) wymagaja potwierdzenia, ze uklad faktycznie gra - patrz punkt 6.
- Narzedzia: Menedzer zadan / Monitor zasobow (pamiec, CPU), Process Monitor (opcjonalnie: dostep do plikow).

---

## 1. Czysta maszyna (bez VC++ redist, bez SDL)

Cel: pojedynczy plik wykonywalny dziala na swiezym systemie bez doinstalowywania czegokolwiek.

### 1.1 Windows 10 22H2 (czysta maszyna lub swiezy snapshot VM)

Przygotowanie: system po instalacji, zaktualizowany; BRAK zainstalowanych "Visual C++ Redistributable" (sprawdz
Aplikacje i funkcje), BRAK SDL3.dll w PATH i obok exe. Skopiuj tylko artefakt wydania do `C:\Users\<uzytkownik>\Desktop\ChipJuke\`.

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Uruchom `ChipJuke.exe` dwuklikiem | Okno startuje bez bledu "brakuje VCRUNTIME140.dll / MSVCP140.dll / SDL3.dll" i bez komunikatu SmartScreen blokujacego uruchomienie na stale ("Uruchom mimo to" jest akceptowalne i odnotowane) | |
| 2. Bez zadnych plikow: obejrzyj ekran startowy | Pusty stan z czytelna podpowiedzia (jak dodac pliki), zadnego crasha | |
| 3. Przeciagnij plik `.vgm` z `D:\emu\vgm` na okno | Utwor sie laduje i gra, czas biegnie, wizualizacje reaguja | |
| 4. Zamknij okno (X) | Proces znika z Menedzera zadan w <2 s; powstal `settings.json` (patrz 2) | |
| 5. Uruchom z konsoli `ChipJuke.exe --diag` | Start z diagnostyka; brak komunikatow o bledach bibliotek | |
| 6. Narzedzia: `dumpbin /dependents ChipJuke.exe` (na maszynie deweloperskiej) | Tylko DLL systemowe (KERNEL32, USER32, D3D11, DXGI, ...); brak `VCRUNTIME*`, `MSVCP*`, `SDL3.dll` | |

Wynik zbiorczy 1.1: ______

### 1.2 Windows 11 (aktualna wersja)

Te same kroki 1-6 co w 1.1, na czystej instalacji Windows 11 bez VC++ redist i bez SDL.

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1-5 jak w 1.1 | jak w 1.1 | |
| 7. Uruchom jako zwykly uzytkownik z folderu pobranych (Downloads, plik z oznaczeniem Mark-of-the-Web) | Aplikacja dziala; ewentualny SmartScreen odnotowany | |
| 8. Sprawdz motyw/ciemny tryb systemu przelaczony w trakcie dzialania | Aplikacja nie przestaje dzialac; wyglad zgodny z ustawieniem aplikacji (nie systemu) | |

Wynik zbiorczy 1.2: ______

---

## 2. Pierwsze uruchomienie: folder zapisywalny i tylko-do-odczytu

Regula (tryb przenosny): ustawienia (`settings.json`, `playlists.json`, `library-cache.json`) leza OBOK `ChipJuke.exe`,
gdy katalog jest zapisywalny; w przeciwnym razie w `%APPDATA%\ChipJuke`.

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 2.1 Pusty, zapisywalny folder (np. `D:\Test\ChipJuke`). Uruchom, zmien jedno ustawienie (np. glosnosc), zamknij | Obok exe powstaje `settings.json` (i `playlists.json` po dodaniu playlisty); `%APPDATA%\ChipJuke` NIE jest tworzony; ponowne uruchomienie pamieta ustawienie | |
| 2.2 Skopiuj folder w inne miejsce (inny dysk / pendrive) | Ustawienia "jada" razem z folderem; aplikacja dziala identycznie | |
| 2.3 `C:\Program Files\ChipJuke\` (zwykly uzytkownik, bez admina): uruchom, zmien ustawienie, zamknij | Brak okna z prosba o uprawnienia administratora; `settings.json` powstaje w `%APPDATA%\ChipJuke\`; ponowne uruchomienie pamieta ustawienie; w `Program Files` nic nowego nie powstalo | |
| 2.4 Folder tylko-do-odczytu (wlasciwosc "Tylko do odczytu" / odebrane prawo zapisu dla uzytkownika) | Jak 2.3; brak bledu zapisu pokazanego uzytkownikowi jako crash; ewentualny komunikat jest zrozumialy | |
| 2.5 Plyta/ISO/udzial tylko-do-odczytu (zamontuj ISO z exe) | Jak 2.3 | |
| 2.6 Istnieje stary `%APPDATA%\ChipJuke\settings.json`, a exe lezy w zapisywalnym folderze bez wlasnego `settings.json` | Zachowanie zgodne z regula (patrz `docs/adr/0003-playlists-settings.md`); odnotuj, ktorego pliku uzyto | |
| 2.7 Migracja nazwy: istnieje `%APPDATA%\vgmplay\` (z wersji testowych), brak `%APPDATA%\ChipJuke`; exe w folderze tylko do odczytu | Po starcie katalog przemianowany na `%APPDATA%\ChipJuke`, ustawienia i playlisty zachowane | |
| 2.7 Uszkodz `settings.json` (obetnij w polowie, uruchom) | Aplikacja startuje z domyslnymi; powstaje kopia `settings.json.bad`; czytelny komunikat; oryginal nie jest po cichu nadpisany bez wiedzy uzytkownika | |
| 2.8 Uszkodz `playlists.json` tak samo | Jak 2.7 (kopia `.bad`), playlisty puste, aplikacja dziala | |
| 2.9 Ustaw w `settings.json` nowsza `schemaVersion` (np. 99) | Aplikacja nie nadpisuje pliku nowszej wersji, informuje o niezgodnosci, dziala na domyslnych | |
| 2.10 `--settings-dir=D:\Test\inne` | Wszystkie trzy pliki powstaja w podanym katalogu | |

Wynik zbiorczy 2: ______

---

## 3. DPI i monitory

### 3.1 Skalowanie DPI

Dla kazdej wartosci skalowania wyloguj sie/uruchom aplikacje po zmianie (Ustawienia > System > Ekran > Skala).

| Skala | Krok | Oczekiwany wynik | Wynik |
|---|---|---|---|
| 100% | Uruchom, otworz wszystkie ekrany (odtwarzacz, playlisty, wzorzec, oscyloskopy, ustawienia, przypisania) | Brak obcietych tekstow, brak rozmytej czcionki, przyciski sa klikalne | |
| 150% | jak wyzej | Czcionka ostra (nie skalowana bitmapowo przez system), proporcje zachowane | |
| 200% | jak wyzej | jak 150%; najmniejszy rozmiar okna nie ucina krytycznych elementow | |
| 125% / 175% (nieparzyste) | Odtwarzacz + playlisty | Brak artefaktow, tekst czytelny | |
| 100-200% | Ustawienia > rozmiar czcionki (`fontScale` 0.5-3.0) na skrajnych wartosciach | Interfejs nadal uzywalny; 3.0 nie wywraca ukladu na malym oknie | |
| 100-200% | Zmiana skali systemu w trakcie dzialania aplikacji | Aplikacja nie crashuje; po restarcie wyglada poprawnie (dopuszczalne: wymaga restartu - odnotuj) | |

### 3.2 Wiele monitorow

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Dwa monitory, rozne skale (np. 100% i 150%): przeciagnij okno miedzy nimi | Brak crasha; rozmiar/ostrosc dostosowuje sie (lub zgodnie z ustaleniem: po restarcie); brak czarnego okna | |
| 2. Zamknij aplikacje na drugim monitorze, odlacz go, uruchom ponownie | Okno pojawia sie na dostepnym monitorze (nie poza ekranem) | |
| 3. Odlacz monitor, na ktorym stoi okno, w trakcie odtwarzania | Okno przenosi sie na pozostaly monitor; dzwiek nie przerywa sie na stale | |
| 4. Tryb pelnoekranowy / maksymalizacja na kazdym monitorze | Poprawny rozmiar, brak paskow/rozciagniecia | |
| 5. Zmiana rozdzielczosci w trakcie dzialania | Brak crasha, ukl. dopasowuje sie | |
| 6. Monitor HDR / z ujemnymi wspolrzednymi (monitor po lewej od glownego) | Okno poprawnie pozycjonowane po restarcie | |

Wynik zbiorczy 3: ______

---

## 4. Pad (kontroler)

Fokus: pad steruje aplikacja TYLKO gdy okno ma fokus (stala decyzja, `input::kGamepadWorksInBackground == false`).

| Urzadzenie / sytuacja | Krok | Oczekiwany wynik | Wynik |
|---|---|---|---|
| Xbox (XInput, kabel) | Nawiguj po playliscie, play/pauza, nastepny/poprzedni, glosnosc, zmiana pasku oscyloskopow | Wszystkie akcje z ekranu podpowiedzi przyciskow dzialaja; podpowiedzi pokazuja symbole Xbox | |
| Xbox bezprzewodowy (Bluetooth/adapter) | j.w. + uspienie pada (wylaczenie) i wybudzenie | Po wybudzeniu steruje dalej bez restartu aplikacji | |
| PlayStation (DualShock 4 / DualSense) | j.w. | Dziala; podpowiedzi pokazuja symbole PlayStation (lub neutralne - zapisz) | |
| Ogolny pad HID (tani, DirectInput) | Ekran przypisan: przypisz kazda akcje do przyciskow pada | Przypisania sie zapisuja (`settings.json` > `input.bindings`) i dzialaja po restarcie | |
| Podlaczanie w trakcie | Uruchom aplikacje BEZ pada, podlacz go w trakcie dzialania | Wykryty w kilka sekund, bez restartu; podpowiedzi przelaczaja sie na pad | |
| Odlaczanie w trakcie | Wyjmij kabel w trakcie nawigacji / przytrzymanej akcji | Brak crasha, brak "zablokowanego" przycisku; klawiatura dziala dalej | |
| Dwa pady naraz | Podlacz dwa pady | Brak crasha; zachowanie zgodne z ustaleniem (odnotuj, czy oba steruja) | |
| Brak fokusu | Uruchom odtwarzanie, przelacz fokus na inna aplikacje, naciskaj przyciski pada | Aplikacja NIE reaguje na pad w tle; po powrocie fokusu reaguje; klawiatura/mysz bez zmian | |
| Przytrzymanie (powtarzanie) | Przytrzymaj kierunek w liscie | Powtarzanie po `repeatDelayMs`/`repeatRateMs`, bez "zacinania" interfejsu | |
| Dryf galek | Pad ze "starymi" galkami (dryf) | Aplikacja nie "chodzi sama" po liscie | |
| Steam Input / DS4Windows w tle | Uruchom emulator XInput | Dziala jak Xbox; brak podwojnych zdarzen | |

Wynik zbiorczy 4: ______

---

## 5. Urzadzenia audio

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 5.1 Ustawienia > urzadzenie audio: lista zawiera urzadzenia systemowe + "domyslne" | Lista bez pustych/duplikowanych pozycji, nazwy czytelne (rowniez polskie znaki w nazwach) | |
| 5.2 Zmiana urzadzenia w trakcie odtwarzania (glosniki -> sluchawki) | Dzwiek przechodzi na nowe urzadzenie bez restartu aplikacji; brak trzasku utrzymujacego sie; pozycja utworu zachowana lub utwor zaczyna sie od poczatku (odnotuj) | |
| 5.3 Odlaczenie urzadzenia USB w trakcie gry | Brak crasha/zawieszenia; komunikat; automatyczne przejscie na domyslne albo czytelny stan bledu z mozliwoscia wybrania urzadzenia | |
| 5.4 Ponowne podlaczenie tego samego urzadzenia | Mozna je wybrac bez restartu; zapamietane id dziala | |
| 5.5 Bluetooth: sluchawki wlaczone przed startem | Dzwiek idzie na BT; opoznienie akceptowalne (odnotuj ms) | |
| 5.6 Bluetooth: wylaczenie sluchawek w trakcie gry | Jak 5.3 | |
| 5.7 Bluetooth: wlaczenie sluchawek w trakcie gry (domyslne urzadzenie systemu zmienia sie) | Aplikacja zachowuje sie spojnie (zostaje na wybranym albo podaza za domyslnym - zgodnie z ustawieniem "domyslne") | |
| 5.8 Zapamietane urzadzenie nie istnieje przy starcie (zmien `audio.device` na nieistniejace id) | Start na domyslnym + komunikat; ustawienie NIE jest po cichu skasowane | |
| 5.9 Czestotliwosc probkowania 44100 / 48000 / 96000 / 0 (urzadzenia) i bufor (`bufferMs`) 10 / 50 / 500 | Dzwiek bez przerw przy 50 ms; przy 10 ms dopuszczalne trzaski (odnotuj); 500 ms bez cisz | |
| 5.10 Tryb wyciszenia / zmiana glosnosci systemowej w trakcie gry | Brak crasha, dzwiek wraca po przywroceniu | |
| 5.11 Uspienie komputera w trakcie odtwarzania i wznowienie | Aplikacja zyje; audio wznawia sie lub daje sie wznowic bez restartu | |
| 5.12 Wyjscie wylacznie wirtualne (np. "Cable Output") lub brak jakiegokolwiek urzadzenia | Brak crasha; czytelny komunikat; aplikacja daje sie zamknac | |

Wynik zbiorczy 5: ______

---

## 6. Pliki VGM: reprezentatywne dla kazdego ukladu

Dla kazdego ukladu otworz plik, odtworz co najmniej 30 s (utwory z petla: przejdz przez petle i fade). Kryteria:
dzwiek bez trzaskow/ciszy "w miejscu muzyki", pozycja i czas biegna, wzorzec (tracker) i oscyloskopy kanalow reaguja
(albo kanaly oznaczone jako "bez dekodera" dla ukladow nieobslugiwanych), MUTE/SOLO kanalu dziala, przewijanie nie psuje.

Kolumna "Plik" - przykladowy plik z `D:\emu\vgm` albo `tests\data\valid`. Uzupelnij konkretna nazwe w wyniku.

| Uklad | Przykladowy plik | Dzwiek | Wzorzec | Oscyloskopy | Wynik |
|---|---|---|---|---|---|
| YM2612 + SN76489 (Mega Drive) | `Streets of Rage 2 ...\01 - Player Select V2.vgz` | | | | |
| SN76489 (sam) | `tests\data\valid\sn76489_*` | | | | |
| YM2151 | `Akumajo_Dracula_(Sharp_X68000)\01 Konami Logo.vgz` | | | | |
| YM2203 | `Dark_Seal_(Arcade)\01 Credit.vgz` | | | | |
| YM2413 | `Lagrange_Point_(Family_Computer)\01 Theme of Isis.vgz` | | | | |
| YM3812 (OPL2) | `Castlevania_(IBM_PC_XT_AT)\01 Prologue.vgz` | | | | |
| YMF262 (OPL3) | `tests\data\valid\ymf262_*` | | | | |
| AY8910 | `Gimmick!_(Family_Computer)\...` / `tests\data\valid\ay8910_*` | | | | |
| NES APU | `Castlevania_(NES)\01 Prologue.vgz` | | | | |
| SegaPCM | `Out_Run_(Arcade)\01 Magical Sound Shower.vgz` | | | | |
| OKIM6258 | `Akumajo_Dracula_(Sharp_X68000)\...` | | | | |
| OKIM6295 | `Dark_Seal_(Arcade)\02 The Legend (Opening).vgz` | | | | |
| C140 | `Dragon_Saber_...\01 Story of Dragon Saber (Opening 1).vgz` | | | | |
| K053260 | `Teenage_Mutant_Ninja_Turtles_-_Turtles_in_Time_(Arcade)\01 Theme from Pizza Power.vgz` | | | | |
| K054539 | `Violent_Storm_(Mystic_Warriors)\01 Opening Demo 1.vgz` | | | | |
| ES5503 / ES5506 / GA20 / SAA1099 / WonderSwan | pliki z `Dark_Seal_(Arcade)` i `Gimmick!_(Family_Computer)` (do potwierdzenia, ktory uklad faktycznie gra) | | | | |
| Dwa uklady tego samego typu ("dual") | `tests\data\valid\dual_*` | | | | |
| Plik `.vgz` (gzip) i `.vgm` | oba rozszerzenia | | | | |
| Plik z GD3 (EN/JP, przelacznik F3) | dowolny z tagami japonskimi | | | | |
| Petla: licznik petli 1/2/5/bez konca, fade 0/1000/5000 ms | utwor z petla | | | | |
| Utwor bez petli (koniec = Stop, bez wyciszania) | `tests\data\valid\*` bez petli | | | | |
| Utwor bardzo dlugi (>10 min) i bardzo krotki (<1 s) | dowolne | | | | |

Pliki niepoprawne (zachowanie bezpieczne):

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| Otworz `tests\data\malformed\*.vgm` (kilka dowolnych) | Czytelny blad "nie mozna zaladowac"; aplikacja dziala dalej; nastepny utwor w playliscie gra | |
| Otworz plik `.txt` zmieniony na `.vgm`, plik 0-bajtowy, plik >100 MB | Blad bez crasha i bez wielosekundowego zawieszenia UI | |
| Otworz pliki z `fuzz\crashes\` (znane, opisane usterki; jesli jeszcze nie naprawione - odnotuj skutek) | Bez crasha procesu (po naprawie); przed naprawa: odnotuj | |

Wynik zbiorczy 6: ______

---

## 7. Playlisty, M3U, sciezki

### 7.1 Duze playlisty

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Dodaj caly katalog `D:\emu\vgm` (skan rekursywny, ~250 plikow) | Skan nie blokuje UI; pasek postepu/liczba plikow; wynik posortowany; po skanie wszystkie utwory na liscie | |
| 2. Wygeneruj playliste ~10 000 wpisow (np. skrypt tworzacy `playlists.json` z kopiami sciezek) i otworz | Wczytanie w rozsadnym czasie (<5 s), przewijanie plynne (>30 fps), brak skokow pamieci >500 MB | |
| 3. Limit 100 000 wpisow na liste / 1000 playlist | Przekroczenie => czytelny komunikat, nie crash | |
| 4. Tasowanie (F7), powtarzanie (F6: wylaczone / jeden / wszystkie), nastepny/poprzedni na 10 000 wpisow | Natychmiastowe; kursor podaza za biezacym utworem | |
| 5. Usuwanie/przenoszenie zakresu wpisow (zaznacz ~1000, usun) | Bez zawieszenia; Cofnij (jesli jest) dziala | |
| 6. Wiele playlist: nowa (F8), zmien nazwe (F9), duplikat nazwy, pusta nazwa | Duplikat/pusta nazwa odrzucona z komunikatem | |
| 7. Zamknij i uruchom ponownie | Aktywna playlista, kursor i pozycja w utworze przywrocone (pozycja tylko gdy sciezka sie zgadza) | |
| 8. Metadane (tytul/czas) wypelniaja sie w tle po skanie | Tytuly pojawiaja sie stopniowo, UI plynne; `library-cache.json` powstaje obok ustawien; drugi start szybszy | |

### 7.2 M3U / M3U8

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Import M3U ze sciezkami wzglednymi (plik w folderze z muzyka) | Pozycje rozwiazane wzgledem folderu M3U; tytuly z `#EXTINF` | |
| 2. M3U8 (UTF-8, z BOM i bez) ze znakami polskimi | Nazwy i tytuly poprawne | |
| 3. M3U w stronie kodowej systemu (Windows-1250) | Poprawne znaki (lub jawny wybor kodowania - odnotuj) | |
| 4. M3U z wpisami `http://`, `file://`, `\\serwer\udzial`, `..\..\poza\folderem` | Pozycje odrzucone z ostrzezeniem; liczba odrzuconych pokazana; reszta zaimportowana | |
| 5. M3U z sciezkami absolutnymi do innego dysku | Odrzucone lub zaakceptowane zgodnie z opcja (domyslnie ograniczenie do katalogu); zachowanie spojne z komunikatem | |
| 6. Eksport playlisty do M3U8 i ponowny import | Roundtrip: te same pozycje, tytuly, kolejnosc | |
| 7. Bardzo duzy M3U (np. 200 000 linii) i M3U >16 MiB | Limit/komunikat zamiast zawieszenia lub OOM | |

### 7.3 Sciezki: polskie znaki i dlugie

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Folder `D:\Muzyka\Zażółć gęślą jaźń\` z plikami `Zażółć.vgm` | Skan, odtwarzanie, zapis w playliscie i ponowne wczytanie bez znieksztalcen; tytul okna/wiersze poprawne | |
| 2. Sciezka >260 znakow (np. 40 zagniezdzonych folderow; wymaga wlaczonego LongPathsEnabled lub prefiksu `\\?\`) | Plik odtwarza sie lub jest czytelny blad; skan nie crashuje | |
| 3. Nazwy z emoji, spacjami na koncu, znakami RTL | Brak crasha; wyswietlanie bez znakow sterujacych zakłócajacych uklad (znaki kierunkowe sa czyszczone) | |
| 4. Przeciagnij-upusc pliku z takimi sciezkami na okno | Dziala jak otwarcie z dialogu | |
| 5. Argument wiersza polecenia: `ChipJuke.exe "D:\Muzyka\Zażółć\a.vgm"` | Otwiera i gra | |
| 6. Powiazanie plikow `.vgm`/`.vgz` w Eksploratorze (otworz za pomoca) | Otwiera w uruchomionej lub nowej instancji (odnotuj zachowanie) | |

Wynik zbiorczy 7: ______

---

## 8. OneDrive i sciezki sieciowe

### 8.1 OneDrive (pliki "tylko online" / placeholdery)

Przygotowanie: folder w OneDrive z kilkoma plikami `.vgm`, czesc ustawiona na "Zwolnij miejsce" (tylko online), czesc "Zawsze zachowaj".

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Skanuj folder OneDrive | Skan NIE pobiera zawartosci plikow "tylko online" (brak aktywnosci pobierania w OneDrive); pliki oznaczone jako placeholder; ostrzezenie "cloud placeholder ..." | |
| 2. Metadane (czas/tytul) dla placeholderow | Pomijane (brak pobierania), wiersz pokazuje brak czasu, UI plynne | |
| 3. Odtworz utwor placeholder wprost ("pobierz mimo to") | Plik jest pobierany i gra po pobraniu; UI nie zawiesza sie podczas pobierania (anulowanie dziala) | |
| 4. Odtworz utwor "Zawsze zachowaj" | Zwykle odtwarzanie | |
| 5. Wylacz siec i otworz placeholder | Czytelny blad (nie zawieszenie, nie crash) | |
| 6. Skan z duzym katalogiem w OneDrive (~1000 plikow) | Brak masowego pobierania; czas skanu rozsadny | |

### 8.2 Sciezki sieciowe (UNC / mapowany dysk)

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Dodaj do playlisty plik z `\\serwer\udzial\a.vgm` (bez `--allow-network-paths`) | Sciezki UNC odrzucone z komunikatem (domyslne bezpieczenstwo) | |
| 2. Uruchom z `--allow-network-paths`; dodaj ten sam plik | Dodaje sie i gra; opcja "zapamietaj zgode" (`library.allowNetworkPaths`) dziala po restarcie | |
| 3. Mapowany dysk (`Z:\muzyka`) | Traktowany jak dysk lokalny (odnotuj: czy wymaga zgody) | |
| 4. Skan duzego katalogu sieciowego (wolne lacze) | UI plynne; anulowanie skanu natychmiast dziala | |
| 5. Rozlaczenie sieci w trakcie odtwarzania z udzialu | Brak crasha; utwor w pamieci dogrywa sie do konca (plik jest wczytywany w calosci) | |
| 6. Odtwarzanie ze sciezki `\\?\` lub `\\.\pipe\...` wpisanej recznie w `playlists.json` | Wpis pominiety (przestrzen urzadzen odrzucana zawsze) | |
| 7. Wpis URL (`http://...`) wpisany recznie w playliste/M3U | Pominiety/odrzucony | |

Wynik zbiorczy 8: ______

---

## 9. Dlugie sesje

### 9.1 Godzinne odtwarzanie (pamiec i CPU)

Przygotowanie: playlista z ~100 roznych utworow z `D:\emu\vgm`, powtarzanie "wszystkie", petle 1, fade 1000 ms. Wizualizacje
wlaczone (wzorzec + oscyloskopy). Monitor zasobow: zapis "Working set (prywatne)", "Uchwyty", "Watki", CPU.

| Moment | Pamiec (MB) | Uchwyty | Watki | CPU w odtwarzaniu (%) | Uwagi |
|---|---|---|---|---|---|
| Start (t=0) | | | | | |
| t=15 min | | | | | |
| t=30 min | | | | | |
| t=45 min | | | | | |
| t=60 min | | | | | |
| Po zatrzymaniu (Stop), t=65 min | | | | | |

Oczekiwane: pamiec stabilizuje sie (wzrost miedzy t=15 a t=60 <10% i bez trendu liniowego), uchwyty/watki stale
(+-5), CPU srednie <15% jednego rdzenia na typowym laptopie (zapisz model CPU); po Stop CPU spada do ~0%; brak zaciec
audio (log: ile przerw uslyszano); zmiana utworu bez przerwy dluzszej niz ustalona.

| Dodatkowe kroki | Oczekiwany wynik | Wynik |
|---|---|---|
| Podczas godzinnego testu: zminimalizuj okno na 10 min, przywroc | CPU w tle niskie (renderowanie wstrzymane/ograniczone), brak skoku pamieci po przywroceniu | |
| Podczas testu: zablokuj ekran (Win+L) na 5 min, odblokuj | Dzwiek gra dalej (jesli tak ma byc), UI wraca bez bledu | |
| Test dlugiego utworu bez konca (petla "bez konca", 1 h na jednym utworze) | Brak narastania pamieci (wzorzec liczony raz), brak rozjazdu pozycji | |
| Uspienie i wznowienie komputera po 30 min | Brak crasha po wznowieniu | |

### 9.2 Zamkniecie w trakcie skanu

| Krok | Oczekiwany wynik | Wynik |
|---|---|---|
| 1. Uruchom skan duzego katalogu (siec/OneDrive lub dysk z >50 000 plikow) | Skan trwa kilka sekund+ | |
| 2. W trakcie skanu zamknij okno (X) | Okno zamyka sie szybko (<3 s); proces znika; brak komunikatu o bledzie; brak procesu-sieroty | |
| 3. Zamkniecie w trakcie skanu przez Alt+F4 / Menedzer zadan "Zakoncz zadanie" | Po ponownym uruchomieniu `settings.json`/`playlists.json` nie sa uszkodzone (zapis atomowy: temp + rename); brak plikow `.tmp*` rosnacych w nieskonczonosc | |
| 4. Zamkniecie w trakcie zapisu playlisty (duzej) | j.w. | |
| 5. Zamkniecie w trakcie odczytu metadanych w tle | Brak zawieszenia na zamknieciu | |
| 6. Anulowanie skanu przyciskiem (jesli jest) i natychmiastowe ponowne uruchomienie skanu | Brak wyscigu: nowy skan startuje, stary nie doklada wynikow | |
| 7. Wylogowanie/zamkniecie systemu przy dzialajacej aplikacji | Aplikacja konczy sie bez blokowania zamkniecia systemu | |

Wynik zbiorczy 9: ______

---

## 10. Podsumowanie

| Sekcja | Wynik zbiorczy | Zgloszenia |
|---|---|---|
| 1 Czysta maszyna (W10 / W11) | | |
| 2 Pierwsze uruchomienie | | |
| 3 DPI / monitory | | |
| 4 Pad | | |
| 5 Audio | | |
| 6 Uklady / pliki VGM | | |
| 7 Playlisty / M3U / sciezki | | |
| 8 OneDrive / siec | | |
| 9 Dlugie sesje / zamkniecie | | |

Decyzja: [ ] wydanie zatwierdzone  [ ] wydanie wstrzymane (powod: ______)  Podpis / data: ______
