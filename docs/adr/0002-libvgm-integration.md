# ADR 0002: Integracja libvgm (silnik odtwarzania)

- Status: propozycja (do akceptacji przez wlasciciela projektu)
- Data: 2026-11-23
- Dotyczy: `src/engine`, `src/format`, `src/loader`, `src/chipstate`, `src/viz`, CMake (`external/libvgm`)
- libvgm: submodul `external/libvgm`, commit `c5c03a2c2514d38d2ab7b7e8dbd021af0b8292cb`
- Metoda: WYLACZNIE czytanie kodu. Nic nie zbudowano ani nie uruchomiono (brak kompilatora/CMake na maszynie).
  Liczby wydajnosci to szacunki do zmierzenia. Oznaczenie [ZALOZENIE] = nie sprawdzone w kodzie.
- Odwolania `plik:linia` sa wzgledem `external/libvgm/`, chyba ze podano inaczej.
- Dowody szczegolowe dla pkt 8: `docs/research/libvgm-parser-hazards.md`. Licencje: `docs/licensing/libvgm-audit.md`.

## Kontekst

Silnik ma renderowac VGM/VGMZ w watku miekkiego RT blokami 256-512 klatek do `PcmRing` (SPSC) i `ScopeRing`
(SPSC); callback miniaudio tylko kopiuje. Sterowanie z UI przez `CmdQueue`, zdarzenia przez `EventQueue`, pozycja
przez atomics. W przyszlosci: eksport WAV, S98 (libvgm ma `S98Player`), NSF/SPC (libvgm ich NIE ma: w `player/` sa
tylko vgm, s98, dro, gym), wiec silnik potrzebuje abstrakcji dekodera. E4 pokaze w siatce trackera nuty i
glosnosci zdekodowane z rejestrow, z przelacznikiem na oscyloskopy per kanal (per kanal pozniej, ale architektura
ma to umozliwic). Domyslnie: 2 przejscia petli, wyciszenie 1 s, oscyloskop sumaryczny L/R w pierwszym wydaniu.

## Decyzje (streszczenie)

1. Uzywamy `PlayerA` (nakladka z petlami/fade/glosnoscia/konwersja) + wlasna podklasa `VGMPlayer`
   (`TapVgmPlayer`), zarejestrowana przez `RegisterPlayerEngine`. Podklasa daje, BEZ lat libvgm, (a) bufory per
   uklad i (b) hook zapisow rejestrow. Zalozenie: wszystkie skladowe po `vgmplayer.hpp:191` sa `protected`
   (potwierdzone), wersja libvgm jest przypieta submodulem.
2. Dekompresje gzip, limity rozmiaru i walidacje robimy MY, przed `MemoryLoader_Init`; libvgm dostaje bufor
   surowy, dopelniony zerami (patrz pkt 8).
3. Per kanal: na E4 NIE renderujemy audio per kanal. Siatka = dekodowane rejestry (hook zapisow). Architektura
   (ScopeBlock/ChannelLayout) rezerwuje strumienie per uklad i per kanal. Pozniej: lata "channel tap" (a) dla 2-3
   najczestszych rdzeni; instancje-cienie (c) tylko jako awaryjne/opcjonalne. Szczegoly w pkt 5.
4. Rdzenie: jawnie ustawiamy `emuCore` dla ukladow, ktorych domyslny rdzen jest wpisany na stale w libvgm, a u nas
   wylaczony (pkt 7).

## 1. Minimalna sciezka odtwarzania

**PlayerA vs VGMPlayer.** `VGMPlayer` (`player/vgmplayer.cpp`) umie tylko: parsowac, emulowac, `Render(n, WAVE_32BS*)`
(`:2035`), `Seek`, `GetTags`, opcje ukladow. Nie ma licznika petli ani fade: gdy callback zdarzen nie zwroci 0x01
na `PLREVT_LOOP`, zapetla sie w nieskonczonosc (`player/vgmplayer_cmdhandler.cpp:572-589`). `PlayerA`
(`player/playera.cpp`) dodaje: wybor silnika po sygnaturze (`:368-384`), licznik petli (`:658-668`), fade
(`:520-544`), cisze koncowa (`:597-609`), glosnosc utworu z naglowka VGM (`:505-518`), konwersje do PCM (`:12-90`),
czas (`:304-356`). Wybieramy `PlayerA`, tak jak `vgm2wav.cpp` i `player.cpp`. Dla przyszlych formatow `PlayerA`
obsluguje tez S98/DRO/GYM (rejestracja kolejnych silnikow, `vgm2wav.cpp:231-234`).

**Sekwencja** (wzorzec `vgm2wav.cpp:229-325`, z MemoryLoader jak `player.cpp:165-172`):

```
PlayerA pa;                                    // watek silnika
pa.RegisterPlayerEngine(new TapVgmPlayer);     // PlayerA przejmuje wlasnosc (delete: playera.cpp:141-143)
pa.SetOutputSettings(sr, 2, 32, maxBlock);     // channels MUSI byc 2 (playera.cpp:154); bits 8/16/24/32
cfg = pa.GetConfiguration(); cfg.loopCount=2; cfg.fadeSmpls=1*sr; cfg.endSilenceSmpls=0; pa.SetConfiguration(cfg);
dl = MemoryLoader_Init(buf, len); DataLoader_Load(dl);   // buf: surowy, juz zwalidowany (pkt 8)
if (pa.LoadFile(dl)) -> blad;                  // playera.cpp:386-401, kody >=0x80 = blad
pa.SetLoopCount(vgm->GetModifiedLoopCount(2)); // jak vgm2wav.cpp:294-298 (vgmplayer.cpp:920-933)
pa.Start();                                    // tworzy urzadzenia (InitDevices, vgmplayer.cpp:1428)
loop: n = pa.Render(frames*8, buf);            // BAJTY, nie klatki (playera.cpp:556)
```

**Zrodlo danych / DATA_LOADER.** `MemoryLoader_Init(const UINT8*, UINT32)` (`utils/MemoryLoader.c:150`) trzyma
wskaznik do naszego bufora tylko do konca wczytywania; `DataLoader_Load` bez `SetPreloadBytes` wczytuje wszystko od
razu (`DataLoader.c:143` `_readStopOfs = -1`, `:65-66`), `VGMPlayer::LoadFile` i tak woluje `ReadAll`
(`vgmplayer.cpp:257`). Dane sa KOPIOWANE do bufora `DataLoader` (`DataLoader.c:105`) i VGMPlayer trzyma wskaznik
`_fileData` do niego przez caly utwor: `DATA_LOADER` musi zyc do `UnloadFile()`, potem `DataLoader_Deinit`.
**libvgm rozpakowuje gzip sam**: `MemoryLoader.c:58-71` (magic 1F 8B, `inflateInit2(0x20|15)`), takze `FileLoader`
(`FileLoader.c:106`, gzopen). **Limitu rozmiaru NIE MA**: rozmiar docelowy pochodzi z ISIZE (ostatnie 4 bajty, `:66`) i
jest alokowany z gory (`DataLoader.c:63`, `:105`); bledy inflate sa ignorowane (`MemoryLoader.c:105-108`).
Decyzja: nasz `loader` (zlib z vcpkg) rozpakowuje z limitem (domyslnie 64 MiB wyniku [ZALOZENIE co do wartosci],
stala konfigurowalna), sprawdza `Z_STREAM_END`, a libvgm dostaje bufor surowy (sciezka "raw" `:73-75`).

**Format probek i czestotliwosc.** `Render(UINT32 smplCnt, WAVE_32BS* data)` (`player/playerbase.hpp:160`):
`WAVE_32BS` = `{INT32 L, R}` (`emu/Resampler.h:13-22`, `DEV_SMPL = INT32` w `emu/snddef.h:7`); skala wewnetrzna
okolo 24 bit ("some cores might output a bit more", `playera.cpp:612`); ukladanie jest ADDYTYWNE (resampler robi
`+=`, `Resampler.c:193`), dlatego `PlayerA::Render` zeruje bufor przed wywolaniem (`:577`). `PlayerA` NIE oferuje
float: `SampleConv_toF32` istnieje (`:70-76`), ale `GetSampleConvFunc` zwraca tylko 8/16/24/32 (`:78-90`).
Droga: `SetOutputSettings(sr, 2, 32, ...)` -> `INT32` obciete do +-0x7FFFFF i przesuniete o 8 (`:58-68`), potem
`float = v / 2147483648.0f` w silniku (wynik identyczny z `v24/2^23`). Odrzucamy ominiecie `PlayerA` i wolanie
`VGMPlayer::Render` wprost (traci fade/petle). Czestotliwosc: dowolna `SetOutputSettings(smplRate,...)`;
`VGMPlayer::SetSampleRate` odmawia w trakcie grania (`vgmplayer.cpp:814-815`), wiec ustawiamy przed `Start`
(przy zmianie urzadzenia audio: `Stop`+`Start`+`Seek`). Czas pliku ma stale 44100 ticks/s
(`:543`, `RefreshTSRates :834-858`), konwersja `Tick2Sample` robi reszte; wewnatrz kazdy uklad pracuje w swojej
natywnej czestotliwosci i jest przeliczany do `sr` resamplerem liniowym (`vgmplayer.cpp:1719`, `Resampler.c:53-88`).
`Render` moze zwrocic mniej niz zadano: obcina do `_smplBuf.size()` (`playera.cpp:575-576`) i przy `PLAYSTATE_FIN`
przerywa, odrzucajac reszte (`:605-608`) - silnik MUSI sprawdzac wartosc zwrotna i dopelniac zerami.
`Render` wolany po koncu zwraca ciszę pelnej dlugosci (`:562-567`).

**Petle i fade: libvgm vs my.**

| Aspekt | libvgm | Nasza decyzja |
|---|---|---|
| Liczba petli | `PlayerA::Config.loopCount` (domyslnie 2, `playera.cpp:97`); 0 = bez konca (`:661`); modyfikator z naglowka VGM tylko przez `GetModifiedLoopCount` (`vgmplayer.cpp:920`) | `loopCount = 2` przez `GetModifiedLoopCount(2)`; 0 = tryb "nieskonczony" w UI |
| Start fade | zdarzenie `PLREVT_LOOP` z `curLoop >= loopCount` -> `FadeOut()` (`playera.cpp:658-667`, `:463-470`); semantyka: czesc petli slychac 2x, fade zaczyna sie na poczatku trzeciego przejscia i wchodzi w nie | uzywamy bez zmian; "2 przejscia" = `loopCount=2` |
| Dlugosc fade | `fadeSmpls` w probkach (domyslnie 0); krzywa kwadratowa (`:537-540`) | `1 * sampleRate` |
| Plik BEZ petli | brak fade; `PLREVT_END` ustawia ogon (`:669-672`), `PLAYSTATE_FIN` po `endSilenceSmpls` | "wyciszenie 1 s" dla plikow bez petli jest niejednoznaczne: ZALOZENIE = 1 s ciszy/ogona (`endSilenceSmpls`), nie fade. Do potwierdzenia przez wlasciciela |
| Koniec | `PLREVT_END` do naszego callbacku DOPIERO przy `FIN` (`:603-604`; wlasny END z playera jest tlumiony `:645`) | callback -> `EventQueue` (`TrackEnded`) |
| Dlugosc do UI | `GetTotalTime(PLAYTIME_LOOP_INCL | PLAYTIME_WITH_FADE | PLAYTIME_WITH_SLNC)` (`:320-342`) | liczymy z wlasnego skanu strumienia (pkt 8), bo naglowkowe `numTicks` jest niezaufane |
| Pauza | `PLAYSTATE_PAUSE` zdefiniowane, `VGMPlayer` go nie obsluguje | pauza = silnik przestaje wolac `Render` (+ miekkie wyciszenie) |

Uwaga: `PlayerA::Seek` zeruje znacznik fade tylko gdy nowa pozycja jest przed nim (`:479-483`); po seeku w obszar
fade silnik musi sam zdecydowac (zalecenie: seek ogranicza sie do `[0, koniec_ostatniej_petli)`).

## 2. Alokacje i I/O w libvgm

| Faza | Alokacje (zrodlo) | I/O |
|---|---|---|
| Konstruktor `VGMPlayer` | `CPConv_Init` (calloc, strdup; `vgmplayer.cpp:201`, `utils/StrUtils-CPConv_Win.c:100-121`) | odczyt REJESTRU (`:43-95`) |
| Load | calloc x2 (`MemoryLoader.c:155,158`), `realloc` bufora (`DataLoader.c:105`), `inflateInit2` (jesli gzip); wektory `_xHdrChip*`, `_devCfgs`, tagi `std::string`, `CPConv_StrConvert` malloc/free (`vgmplayer.cpp:494-497`); `ParseFileForOPL4ROMRequirement` malloc 0xC00 (`:1806`) | brak (z MemoryLoader) |
| Start | `InitDevices` (`:1428`): `SndEmu_Start2` (kazdy rdzen malloc/calloc stanu, ROM), `SetupLinkedDevices` calloc (`player/helper.c:26`), `Resmpl_Init` malloc 1 s probek chipu x 2 kanaly x 4 B (`Resampler.c:100-103`, `:26`; np. YM2612 ~53,3 kHz => ~0,4 MB/uklad; SN76496 ok. 220 kHz => ~1,8 MB) | `LoadOPL4ROM` -> callback `FileReq` po `yrw801.rom` (`:1934-1972`), jesli ustawiony |
| Seek wstecz | `Reset()` (`:1009`): `clear()` wektorow banku PCM, `free` tabeli, NIE realokuje ukladow | brak |
| Render | `PlayerA::Render` tylko `memset` (`:577`); `VGMPlayer::Render` bez alokacji **w ustalonym stanie** | brak |
| Render, wyjatki | w `ParseFile`: blok danych 0x67 (`resize`, `push_back`, `memcpy`; `cmdhandler:725-744`), tabela kompresji (`realloc`, `dblk_compr.c:751`), bufor `swpData` (`cmdhandler:766`), zapis ROM => `realloc` w rdzeniu, `Cmd_DACCtrl_Setup` calloc + `push_back` (`cmdhandler:880-900`), logi `emu_logf` (stos 256 B, `emu/logging.c:20`) -> nasz callback, `Resmpl_EnsureBuffers` gdy zadanie > pojemnosc (`Resampler.c:17-31`, tu `abort()` przy OOM) | brak |

**Czy `Render()` na zwyklej sciezce jest wolny od alokacji?** Tak, o ile strumien nie zawiera w srodku utworu
blokow 0x67, zapisow ROM/RAM ani `0x90` (Setup DAC). Zwykle sa na poczatku pliku, przed pierwszym oczekiwaniem.
Zalecenie: po `Start()` wykonac "dummy-render" `pa.Render(0, nullptr)` - `PlayerA` przekazuje go jako
`_player->Render(0, NULL)` (`playera.cpp:569-572`), a `VGMPlayer::Render` przy `smplCnt==0` tylko parsuje komendy do
biezacej probki (`vgmplayer.cpp:2043-2056`, `Resmpl_Execute` z 0 wraca od razu, `Resampler.c:479`). Dzieki temu
poczatkowe bloki danych laduja sie w kroku Load (poza sciezka czasu rzeczywistego). Silnik jest "miekki RT", wiec
reszte alokacji (rzadkie bloki w srodku) tolerujemy, ale: callback logow libvgm NIE moze alokowac ani blokowac
(zapis do pierscienia ostatnich N komunikatow), a wyjatek `std::bad_alloc` z `VGMPlayer` musi byc zlapany na
granicy silnika (`try/catch` wokol `Render/Seek/Load`, zdarzenie bledu zamiast `terminate`).
Wszystkie wolania libvgm tylko z JEDNEGO watku silnika (libvgm nie jest watkowo-bezpieczny; callbacki leca
synchronicznie wewnatrz `Render`).

## 3. Seek

`PlayerA::Seek(PLAYPOS_SAMPLE, pos)` (`playera.cpp:472`) -> `VGMPlayer::Seek` (`vgmplayer.cpp:1976-1997`): konwersja
probki na tick; gdy cel jest PRZED biezaca pozycja -> `Reset()` (reset ukladow, wyczyszczenie banku PCM,
`_filePos = dataOfs`, `:1009-1078`); potem `SeekToTick` (`:1999-2007`) = `ParseFile(tick - _playTick)`.
**To NIE jest renderowanie na sucho**: `ParseFile` tylko wykonuje komendy (zapisy rejestrow, bloki danych),
bez wolania `Update` rdzeni, wiec koszt to O(liczba komend do celu), nie O(probki). Seek w przod (cel > biezaca
pozycja) nie robi resetu i dziala od biezacego miejsca.

Koszt [SZACUNEK, niezmierzony]: zwykly utwor (rzedu 10^5 - 10^6 komend/5 min) - kilka do kilkudziesieciu ms;
najgorszy przypadek: pliki z PCM przez komendy `0x80-0x8F` (zapis YM2612 DAC co probke, ~44 100 komend/s) - rzad
10^7 komend / 5 min => ok. 0,3-1 s przy ~30-100 ns/komende. Wymaga benchmarku (zadanie dla E3).

Skutki uboczne, o ktorych trzeba wiedziec:
- stan zalezny od CZASU RENDEROWANIA nie jest odtwarzany: obwiednie, LFO, szum SN, pozycje ADPCM/PCM, timery.
  Trzymane nuty FM zaczynaja po seeku od ataku; strumienie DAC (`0x90-0x95`) po seeku startuja od poczatku
  probki (strumienie ida tylko w `Render`, `:2071-2075`). Zwykle akceptowalne; do opisu w UI/dokumentacji.
- stan resamplerow nie jest resetowany (`vgmplayer.cpp:1062` TODO) - drobna nieciaglosc, maskowana mikro-fade.
- komendy petli (`Cmd_EndOfData`) wola callback `PLREVT_LOOP` takze podczas seeka (`cmdhandler:560-589`).
- `PLAYSTATE_SEEK` tlumi logi poziomu > ERROR (`vgmplayer.cpp:955`).

Przyspieszenie: (1) rdzenie nie maja API zapisu/odczytu stanu (`DEV_DEF`, `emu/EmuStructs.h:101-121`), wiec
punkty kontrolne (snapshot co N s) niemozliwe bez lat; (2) seek wstecz o kilka s mozna robic jako `Reset` +
replay, tak jak teraz; (3) nasz wlasny skan strumienia (pkt 8) moze zbudowac indeks tick -> offset (co ~1 s),
uzyteczny do UI, ale NIE przyspiesza seeka (komendy do celu i tak musza byc wykonane); (4) skrocenie pracy:
w tym samym skanie wykryc, ktore zapisy sa nadpisywane - zbyt zlozone, odrzucone. Wniosek: zostaje jak jest,
mierzymy; mikro-fade po stronie silnika (pkt 10, "Seek i PcmRing").

## 4. Mute/solo

API (`player/playerbase.hpp`): `UINT8 SetDeviceMuting(UINT32 id, const PLR_MUTE_OPTS&)` (`:130`),
`GetDeviceMuting` (`:131`), `SetDeviceOptions/GetDeviceOptions` (`:128-129`), `static InitDeviceOptions` (`:127`).
`PLR_MUTE_OPTS { UINT8 disable; UINT32 chnMute[2]; }` (`:77-81`): `disable` = maska zawieszenia emulacji (0x01 uklad
glowny, 0x02 uklad polaczony, 0xFF wszystko); `chnMute[0]` maska kanalow ukladu glownego, `chnMute[1]` ukladu
polaczonego (SSG w YM2203/2608/2610, FM w OPL4); bit 1 = wyciszony (np. `emu/cores/fmopn.c:4783-4791`).
`id`: `PLR_DEV_ID(type, instance)` (`:87`, bit 31 ustawiony; rozkodowanie `vgmplayer.cpp:669-673`) dziala takze
PRZED `Start`; liczba bez bitu 31 = indeks w `_devices`, wazna po `Start`. Implementacja
`VGMPlayer::SetDeviceMuting` (`:775-787`) zapisuje w `_devOpts` i wola `RefreshMuting` (`:712-725`) ->
`devDef->SetMuteMask(dataPtr, mask)` dla ukladu i max. jednego polaczonego (`linkCntr < 2`).
Liczbe i nazwy kanalow daje `DEV_DECL::channelCount/channelNames` (`emu/EmuStructs.h:126-127`), np. YM2612: 7
(FM1..FM6, DAC; `emu/cores/2612intf.c:152-168`). Kanaly wg `SndEmu_GetDevDecl(type, NULL, 0)`.

Pulapki: (a) `disable` pomija `Resmpl_Execute` (`vgmplayer.cpp:2062-2069`) - uklad NIE jest emulowany (zamraza stan,
timery, obwiednie); do solo/mute uzywac `chnMute`, a `disable` tylko do trybow awaryjnych. (b) `_devOpts` zyje
dluzej niz utwor (`UnloadFile` go nie czysci, `:501-520`) - silnik MUSI zerowac maski przy kazdym Load. (c) rdzen
bez `SetMuteMask` (`NULL`, `:720`) ignoruje wyciszenie; nie sprawdzono, ktore rdzenie tak maja [ZALOZENIE: do
tabeli w teste]. (d) w GPGX wyciszony kanal jest calkiem pomijany w `chan_calc` (`fmopn.c:1635`) - jego
obwiednie/fazy nie ida, po odciszeniu brzmi inaczej niz w ciaglym graniu (akceptowalne). (e) Solo realizujemy po
swojej stronie: maska = wszystko poza wybranym; stan mute/solo trzymany w silniku, nie w libvgm.

## 5. Bufor per uklad / per kanal, warianty, rekomendacja

**Fakty.** Publiczne API nie daje buforow per uklad ani per kanal: `VGMPlayer::Render` wola dla kazdego ukladu
`Resmpl_Execute(&clDev->resmpl, step, &data[curSmpl])` do WSPOLNEGO bufora (`vgmplayer.cpp:2059-2070`); rdzenie
same sumuja kanaly do dwoch wyjsc (`DEVFUNC_UPDATE(info, samples, DEV_SMPL** outputs)`, `EmuStructs.h:36`; np.
`chan_calc` dodaje do wspolnych akumulatorow, `fmopn.c:1629-1700`).

**(b) Osobne renderowanie per urzadzenie - wykonalne BEZ lat.** `VGMPlayer::Render` nie jest dla nas swieta:
`PlayerBase::Render` jest wirtualne (`playerbase.hpp:160`), a stan (`_devices`, `_devOpts`, `_dacStreams`,
`_playSmpl`, `ParseFile`, `Sample2Tick`) jest `protected` (`vgmplayer.hpp:191-379`). `TapVgmPlayer::Render` to
kopia ~50 linii (`:2035-2087`), w ktorej `Resmpl_Execute` idzie do bufora scratch danego ukladu, a potem jest
dodawany do sumy. Koszt CPU: zerowy dla emulacji + 1 dodatkowe zerowanie i dodanie bufora na uklad i blok (rzad
2 x n x 8 B; pomijalny). Koszt utrzymania: kopia funkcji do przegladu przy kazdej zmianie submodulu (przypiety
commit + test "rownowaznosci": suma buforow per uklad == wyjscie `VGMPlayer::Render` bit w bit na korpusie).
Efekt: oscyloskop per UKLAD (SN76496, YM2612...) bez dotykania rdzeni. `muteOpts.disable` rozpoznaje te same
warunki jak oryginal.

**(a) Lata "channel tap" w rdzeniach.** W kazdym rdzeniu trzeba wyprowadzic wyjscie per kanal (dodatkowy
`DEV_SMPL* chnOut[]` lub callback), a `DEVFUNC_UPDATE` nie ma na to miejsca => nowe pole w `DEV_DEF` (np.
`SetChannelTap`) i zmiany w kazdym rdzeniu z ktorego chcemy kanaly. Zakres: ok. 25 rdzeni x 2-8 kanalow;
zrobic mozna wybiorczo (YM2612 GPGX `fmopn.c`, SN76496 MAME, YM2151, AY8910, YM2413, NES, GB, HuC6280). Koszt CPU:
niski (zapis jednej probki na kanal; kanaly i tak sa liczone osobno w `chan_calc`, trzeba tylko nie scalac
od razu) - szacunek, nie mierzone. Koszt utrzymania: WYSOKI i trwaly: rebase lat na kazda aktualizacje
submodulu, a submodul jest zalezny od upstreamu (`docs/licensing/libvgm-audit.md` - brak LICENSE w repo; patche
to kod pochodny GPL). Zasada projektu: nie modyfikujemy `external/libvgm`; lata = seria `.patch` stosowana w
CMake lub fork, zatwierdzenie przez wlasciciela.

**(c) Instancje-cienie z maska mute.** Dla kanalu K tworzymy dodatkowa instancje ukladu (ten sam rdzen, te same
zapisy replikowane przez hook z pkt 6) z `chnMute` = wszystko poza K. Bez zmian w libvgm. Koszt CPU: dla ukladu o N
kanalach ~ (1 + N) x koszt pelnej emulacji ukladu, minus oszczednosc z pomijania wyciszonych kanalow w `chan_calc`
(GPGX tak robi, `fmopn.c:1635`; Nuked liczy wszystko) => realistycznie 2-4x dla GPGX, ~N+1 dla Nuked
[SZACUNEK, niezmierzony]. Dla ukladow z kilkoma instancjami/kanalami (np. YM2612 + SN76496 + PCM) kilkanascie
dodatkowych instancji. Dodatkowo: (i) cien utworzony w polowie utworu rozjedzie sie z glownym (nie ma snapshotu
stanu rdzenia - pkt 3), wiec musi byc renderowany od poczatku utworu; (ii) strumienie DAC (`0x90+`) pisza do
rdzenia przez `SndEmu_GetDeviceFunc` z pominieciem naszego hooka (`emu/dac_control.c:408-415`), wiec cien tego nie
zobaczy bez dodatkowej pracy; (iii) sumy cieni != suma glownego przy ograniczeniach/clipie. Koszt utrzymania:
niski w kodzie, wysoki w CPU i zlozonosci synchronizacji.

**Rekomendacja na E4:**
1. Audio per kanal NIE wchodzi do E4. Siatka trackera zasilana jest zdekodowanymi nutami/glosnosciami z
   rejestrow (pkt 6). Przelacznik "oscyloskopy w kolumnach" w E4 pokazuje, jesli dostepne, wykres per UKLAD
   (wariant b) w kolumnie danego ukladu; dla kanalow bez wlasnego strumienia pokazuje wykres ukladu lub
   "brak" - wybor UX do ustalenia z wlascicielem.
2. Wariant (b) wchodzi w E3/E4 jako element silnika (maly koszt, bez lat); pierwsze wydanie moze jednak ograniczyc
   sie do sumy L/R (decyzja uzytkownika) - architektura niesie juz strumienie per uklad.
3. Per kanal (wlasciwe) po E4: wariant (a) dla kilku rdzeni, zaczynajac od YM2612 (GPGX) i SN76496 (MAME);
   (c) tylko jako tryb opcjonalny/diagnostyczny z ostrzezeniem o CPU. Decyzja o (a) wymaga: profilowania, zgody na
   patch-series, testow rownowaznosci (wyjscie z lat bit-w-bit takie samo jak bez lat).
4. W `ScopeBlock` i `ChannelLayout` (pkt 10) rezerwujemy tablice strumieni z identyfikatorem `(deviceIdx,
   channelIdx)`; `channelIdx = 0xFFFF` = suma ukladu; brak strumienia = flaga niedostepnosci.

**Decyzja E1.5 (2026-11-24, uzytkownik): per kanal = wariant (c), bez lat libvgm.** Zastepuje punkty 1 i 3
powyzszej rekomendacji w czesci "per kanal" (oscyloskopy FT2 i VU per kanal wchodza). Wlaczane komenda
`cmd::SetChannelScopes{true}`; wylaczone = zero kosztu (kopie zniszczone, `extraStreamCount = 0`).

- **Implementacja:** `src/engine/channel_scopes.cpp` (`ChannelScopeBank`). Dla kazdego rozdzielalnego kanalu
  osobny `IDecoder` (ten sam bufor, ta sama `DecoderConfig`, ten sam rdzen), maska MUTE = wszystko poza kanalem,
  a WSZYSTKIE INNE urzadzenia sa "zamrozone" (`muteOpts.disable` - `Render` pomija `Resmpl_Execute`, zapisy
  rejestrow nadal przyjmowane; `vgmplayer.cpp:2059-2070`). Kopia kosztuje wiec emulacje JEDNEGO ukladu, nie calego
  utworu. Zamrozenie dotyczy tylko kopii (glowny dekoder nigdy - pkt 4a). Zastrzezenia (i),(ii) z opisu wariantu (c)
  nie wystepuja: kopia to caly odtwarzacz (zapisy DAC-stream tez), a seek nie renderuje (tylko zapisy), wiec po
  seeku kopia i glowny dekoder maja IDENTYCZNY stan.
- **Synchronizacja:** wszystko na watku silnika. Load / reconfigure / Seek / Stop: `invalidate()` i zaraz
  `syncStep` (budzet 100 ms, przed ponownym wlaczeniem audio / pierwszym blokiem): kopia robi `seek(renderPos)`
  tym samym kodem co glowny dekoder (przesuniecie pozycji tolerowane do 8 klatek). Kazdy blok: render glowny ->
  te same `n` klatek w kazdej kopii (takze ogon po Pause i bloki bez scope). Rozjazd (inna liczba klatek /
  pozycja) = ponowna synchronizacja; po 4 rozjazdach kopia uznana za niedostepna. Wlaczenie w polowie utworu:
  kopie tworzone stopniowo (3 ms na iteracje petli) i przewijane do pozycji; startuja ze stanu "po seeku", wiec
  FAZY oscylatorow moga sie roznic od mastera (suma != master do nastepnego key-on) - ograniczenie przyjete
  (renderowanie od zera kosztowaloby sekundy CPU). Glowny miks nie pochodzi z kopii (golden bez zmian).
  MUTE/SOLO dotycza tylko mastera - strumien wyciszonego kanalu nadal ma przebieg (jak FT2).
- **Czestotliwosc: ta sama co master (bez decymacji).** Pomiar: emulacja idzie z natywna czestotliwoscia ukladu,
  a nizsze wyjscie oszczedza tylko resampler: master @24 kHz 55,6 ms vs 62,3 ms @48 kHz (20 s utworu), -11%. To nie
  uzasadnia dryfu faz, przeliczania pozycji i interpolacji. `extra[i][j]` lezy w tej samej siatce klatek co
  `masterL/R` (klatka `startFrame + j`), wartosc = (L+R)/2 kopii.
- **Wydajnosc (release, MSVC x64, jeden watek, render offline 20 s @48 kHz):** Dark Seal `05 ... (Stage 1, 3).vgz`
  (YM2151 8 kanalow + OKIM6295 4 = 12 kopii): master 62 ms (0,31% CPU), master + 12 kopii 351 ms (1,76% CPU),
  utworzenie 12 kopii 5,8 ms. Na zywo (Engine + PcmRing + odbior scope, caly utwor ~6,4 min, 8x czas
  rzeczywisty): underruns = 0, master identyczny z renderem offline (diff_rms = 0). Osobny watek roboczy
  niepotrzebny (a libvgm nie jest udokumentowany jako bezpieczny dla wielu watkow). Korpus tests/data/valid: suma
  strumieni == master bit w bit (blad 0) dla SN76489, AY8910, YM2612 (+DAC), YM2151, YM2413, YM3812, YMF262 i
  dual-chip - mieszanie jest liniowe calkowitoliczbowe.
- **Limity:** `kMaxExtraStreams` 16 -> 32 (ScopeBlock ~68 KiB, pierscien ~2,1 MiB; semantyka bez zmian). Strumienie
  wybierane po kolei (rozdzielalne najpierw); ponad limit - brak strumienia. Budzet pamieci kopii 384 MiB
  (~2 x rozmiar rozpakowanego utworu + 2 MiB na kopie); ponad budzet - mniej strumieni.
- **Kanaly nierozdzielalne:** rdzenie bez `SetMuteMask` (sprawdzane w czasie dzialania:
  `IDecoder::channelIsolatable`) dostaja wpis `flags == 0` (jesli jest miejsce). W korpusie testowym wszystkie
  kanaly sa rozdzielalne; pelnej listy rdzeni bez `SetMuteMask` nie zestawiano.

## 6. Hook zapisow rejestrow

libvgm NIE ma callbacku ani logu komend. Callbacki `PlayerBase`: zdarzenia START/STOP/LOOP/END
(`playerbase.hpp:25-30`), zadanie pliku (`:32`), log tekstowy (`:34`); zapisy rejestrow nie sa logowane. Handlery
komend wolaja wskazniki funkcji z `CHIP_DEVICE` (`write8`, `writeM8`, `writeD16`, `writeM16`, `romWrite`;
`vgmplayer.hpp:60-79`), np. `SendYMCommand` (`cmdhandler:638-643`). Warianty:

- **A. Wlasny rownolegly parser strumienia** (kopia tabeli komend, mapowanie na uklady, pozycja probki z tickow):
  duplikuje ~250 linii wiedzy o formacie, ryzyko dryfu; plus: niezalezne od wnetrzosci libvgm, daje dokladna
  pozycje komendy. Tabele `_CMD_INFO` mozna uzyc bez kopiowania przez podklase (`protected static`, `vgmplayer.hpp:348`).
- **B. Trampoliny na wskaznikach `CHIP_DEVICE`** (REKOMENDACJA): `TapVgmPlayer::Start()` po `VGMPlayer::Start()`
  podmienia `write8/writeM8/writeD16/writeM16` w `_devices[i]` na nasze funkcje (podpis zawiera `void* info`, z
  niego po `dataPtr` odnajdujemy `(vgmChipType, chipID)`), ktore wolaja oryginal i wpisuja zdarzenie
  `{sampleIdx, chipType, chipId, addr/port, data}` do `RegWriteRing`/`ChipState`. Znacznik czasu: `_playSmpl` z
  poczatku kroku `Render` (rozdzielczosc = krok renderu, wystarcza do siatki). Zaleta: dokladnie to, co chip
  dostaje, takze po wszystkich lataniach libvgm (np. NES FDS remap, `cmdhandler:1292-1310`; QSound
  `cmdhandler:1222-1264`); komendy YM (`SendYMCommand`) przychodza jako DWA zapisy `write8` (adres/dane) -
  dekoder `chipstate` skleja pary. Ograniczenia: (1) strumienie DAC (`0x90-0x95`) pisza do rdzenia
  bezposrednio przez `SndEmu_GetDeviceFunc` (`dac_control.c:408-415`) - ich zapisow nie zobaczymy (dla nut/glosnosci
  zwykle nieistotne, dla PCM z DAC-stream tak); (2) zapisy ROM/RAM (`romWrite`) pomijamy; (3) polaczone uklady
  (SSG w OPN) dostaja zapisy przez uklad nadrzedny - widzimy je jako port rejestrow rodzica; (4) podczas seeka zapisy
  tez przechodza przez trampoliny: aktualizujemy stan rejestrow (to daje DOKLADNY stan siatki po seeku),
  ale NIE wpisujemy do `RegWriteRing` (rozpoznawanie przez `GetState() & PLAYSTATE_SEEK`, `playerbase.hpp:15`);
  (5) zapisy `0x2A` z komend `0x80-0x8F` to ~44 100/s na uklad - filtrowac/agregowac przed pierscieniem.
  Zalezy od wnetrzosci `protected` przypietego commitu (test kompilacji + test rownowaznosci wyjscia).
- **C. Lata w `vgmplayer_cmdhandler.cpp`** (callback w kazdej `Cmd_*`): odrzucone (zasada "bez lat", wysoki
  koszt utrzymania).

Rekomendacja: **B**, a **A** jako zapasowa gdyby okazalo sie, ze `protected` cos blokuje (kompilacja niesprawdzona).

## 7. Wybor rdzeni

API: `PLR_DEV_OPTS.emuCore[0]` (rdzen glowny) i `[1]` (rdzen ukladu polaczonego) = FCC rdzenia, 0 = domyslny
(`playerbase.hpp:89-98`). Ustawienie: `PlayerBase::GetDeviceOptions(PLR_DEV_ID(DEVID_YM2612,0), o); o.emuCore[0] =
FCC_NUKE; SetDeviceOptions(...)` (`vgm2wav.cpp:400-410`); odczytywane PRZY `Start` (`vgmplayer.cpp:1478`), wiec zmiana
rdzenia wymaga `Stop/Start` (+ `Seek` do poprzedniej pozycji). Instancje 0/1 maja osobne opcje (`_devOpts[...][2]`).
Stale FCC: `emu/EmuCores.h` (`FCC_GPGX`, `FCC_GENS`, `FCC_NUKE`, `FCC_MAME`, ...). Lista rdzeni ukladu do UI:
`SndEmu_GetDevDecl(type, NULL, 0)->cores[]` (`emu/SoundEmu.h:28`, `EmuStructs.h:122-130`; `name`, `author`, `coreID`) albo
przestarzale `SndEmu_GetDevDefList` (`SoundEmu.h:20`, uzywane w `vgm2wav.cpp:444`).

Regula domyslna: gdy `emuCore == 0`, wybierany jest PIERWSZY rdzen z tablicy `cores[]` danego ukladu (`emu/SoundEmu.c:376-393`),
czyli kolejnosc w `*intf.c`, a NIE rdzen oznaczony `*` w `CMakeLists.txt` (np. SAA1099: kolejnosc VB, MAME, wiec domyslny VB,
choc CMake oznacza MAME gwiazdka; `emu/cores/saaintf.c`). Dodatkowo `VGMPlayer` wpisuje na stale (`vgmplayer.cpp`):
SN76496=MAME (`:1501`), RF5C68=MAME / RF5C164=GENS (`:1508-1515`), QSound=CTR (`:1575`), polaczone AY8910=EMU2149 i
YMF262=AdLibEmu (`:1766-1769`, nadpisywane przez `emuCore[1]`, `:1759-1762`).

**Wplyw audytu licencji (`docs/licensing/libvgm-audit.md`, korzenny `CMakeLists.txt`):**
- `SNDEMU_C6280_OOTAKE=OFF`: HuC6280 ma tylko rdzen MAME (`c6280_mame.c`); `emuCore=0` dziala.
- `SNDEMU_YM3812_ADLIBEMU=OFF`, `SNDEMU_YMF262_ADLIBEMU=OFF`: domyslny YM3812/YMF262 = pierwszy z listy = MAME
  (`oplintf.c`/`262intf.c`); dostepny Nuked. **PULAPKA:** dla YMF278B (OPL4) polaczony YMF262 jest wymuszany na
  `FCC_ADLE` (`vgmplayer.cpp:1768-1769`); skoro adlibemu nie jest skompilowany, `SndEmu_StartCore` zwroci
  `EERR_NOT_FOUND` (`SoundEmu.c:392`) i `SetupLinkedDevices` porzuci czesc FM (`helper.c:39-45`) - OPL4 bez FM. Silnik
  MUSI ustawic `emuCore[1] = FCC_MAME` (lub `FCC_NUKE`) dla `DEVID_YMF278B` (pierwszenstwo `:1759-1762`).
  Test: plik VGM z OPL4 musi dac nie-cisze (wymaga `yrw801.rom`, jesli uzywa ROM-u FM).
- tryb ostrozny `VGMPLAY_LIBVGM_STRICT_LICENSES=ON` (Gens `ym2612.c`/`scd_pcm.c`, `qsound_ctr.c`, Maxim wylaczone wg
  audytu): wtedy QSound (domyslnie CTR) i RF5C164 (domyslnie GENS) NIE wystartuja bez nadpisania. `emuCore` ma byc
  ustawiony na `FCC_MAME` dla QSound i RF5C68/164 niezaleznie od trybu (jedno miejsce w `engine`: tabela
  "domyslne rdzenie vgmplay").
- `REALLY_NEED_CXX14=OFF`: brak rdzenia ymfm (YM2414/OPZ), brak `ymfmintf.cpp` (`emu/CMakeLists.txt:669-682`);
  `YM2414` nie wystepuje w `_DEV_LIST` (`vgmplayer.cpp:49-57`) - bez skutku dla VGM.

**Domyslne rdzenie vgmplay (propozycja):** YM2612 = GPGX (`FCC_GPGX`; najlzejszy, zgodny z trybem "legacy" dla
Project2612, `vgmplayer.cpp:702-703`), opcja "wysoka dokladnosc" = Nuked (`FCC_NUKE`; `vgm2wav.cpp:307` ostrzega o
duzym CPU); YM2413 = EMU2413 (domyslny libvgm); YM2151 = MAME; SN76496 = MAME; AY8910 = EMU2149; YM3812/YMF262/YMF278B =
MAME (`emuCore[1]` jw.); NES = NSFPlay (zalezne od decyzji licencyjnej, audyt poz. 3); GB = SameBoy; HuC6280 = MAME;
QSound = MAME; SAA1099 = wg decyzji (VB lub MAME). Ustawienia per uklad zapisujemy w `settings` (JSON), UI wybiera z
listy `cores[]`.

## 8. Bezpieczenstwo parsera: co MUSIMY zwalidowac sami

Co libvgm waliduje: `eofOfs` (poprawia do rozmiaru, ale porownuje z rozmiarem BUFORA Z PADDINGIEM - patrz pkt 17),
`loopOfs` w `[dataOfs, dataEnd)` i `loopTicks != 0` (`:340-353`), GD3 (offset, sygnatura, wersja, `eotPos`, `:439-459`;
wynik zreszta ignorowany), rozszerzony naglowek (czesciowo: `:382`, `:391`, `:408`, `:417`, ale odczyt `extraHdrOfs`
moze siegnac za plik), `Cmd_PcmRamWrite` (`cmdhandler:824-833`), `ParseFileForOPL4ROMRequirement` (`:1833`).
**Sprostowania wczesniejszych twierdzen** (ta sekcja mowila, ze libvgm waliduje `daccontrol_start`; zrodlo tego nie potwierdza):
* `daccontrol_start` (`emu/dac_control.c:528-534`) NIE chroni przed zerem: `1000 * Length / Frequency` (tryb MSEC),
  `(DataLen - ...) / DataStep` (TOEND), `Length / DataStep` (BYTES) - dzielenie przez zero (#DE, SEH 0xC0000094 w buildzie
  MSVC; sprawdzone na buildzie wydaniowym dla MSEC, TOEND, BYTES oraz 0x95). Sprawdzane jest tylko `Running & 0x80`.
* konfiguracja ukladow (`GenerateDeviceConfig`, `vgmplayer.cpp:1253-1426`): `devCfg.clock * 72 / _hdrBuffer[0xD6]` (C352,
  `:1374`) dzieli przez bajt z pliku bez ochrony; mnozenia zegarow (MultiPCM `*224/180`, C352 `*72`, K054539 `*384`...)
  sa w UINT32 z zawinieciem.
* `Resmpl_Init` (`emu/Resampler.c:103`) alokuje 1 s probek (rate * 2 * 4 B) i `abort()`-uje przy OOM; rate zalezy od
  zegara (POKEY, K005289: rate = zegar), a zegar nie ma zadnej gornej granicy.
Co NIE jest sprawdzane - luki H1-H9 w `docs/research/libvgm-parser-hazards.md`.

**Lista kontroli wykonywanych przez nas PRZED `MemoryLoader_Init`** (modul `format`/`loader`; wszystkie funkcje
czyste, testowalne, celem fuzzingu):

1. Rozmiar wejscia (skompresowanego i surowego) <= limit (domyslnie 64 MiB po dekompresji [ZALOZENIE]).
2. gzip: rozpakowanie WLASNYM zlib ze sprawdzeniem `Z_STREAM_END`, limitem wyjscia (bez ufania ISIZE), odrzucenie
   bledow/uciecia; wynik: bufor surowy. Bufor zaczynajacy sie od 1F 8B po rozpakowaniu = odrzucic.
3. Sygnatura "Vgm " i rozmiar >= 0x40 (wersja < 1.50) lub >= `dataOfs` (>= 0x38) i nie mniej niz
   `min(dataOfs, 0x100)`; wersja BCD sensowna (np. 0x100..0x2FF, nieznane wyzsze = ostrzezenie).
4. `dataOfs` (wzgledne!) bez zawiniecia UINT32: `0x34 + rel` liczone w UINT64, `<= rozmiar`; dla wersji < 1.50 stale 0x40.
5. `eofOfs` (`0x04 + rel`) w UINT64, `<= rozmiar`; (libvgm sam poprawia, ale my chcemy odrzucic/zalogowac niespojny).
6. `gd3Ofs`, `loopOfs`, `extraHdrOfs`, `xhChpClkOfs`, `xhChpVolOfs`: kazdy offset wzgledny w UINT64; `+ 12` (GD3),
   `+ 4/8/12` (extra header) miesci sie w rozmiarze; `loopOfs` w `[dataOfs, dataEnd)`.
7. Dopelnienie bufora: kopia z doklejonymi >= 0x110 zerowymi bajtami ZA koncem pliku (pokrywa H1/H2: memcpy
   naglowka 0x100 B i operandy ostatniej komendy). Naglowkowe `eofOfs` pozostaje wzgledem prawdziwego rozmiaru
   (`dataEnd` nie wchodzi w dopelnienie). Uwaga: `DataLoader_GetSize` zobaczy dlugosc z dopelnieniem.
8. **Pre-skan strumienia komend** od `dataOfs` do `dataEnd` z tabela dlugosci komend (uzyc `_CMD_INFO` przez podklase,
   zeby nie duplikowac: `vgmplayer.hpp:348`): (a) kazda komenda miesci sie w `dataEnd`; (b) nieznane/niewazne komendy
   (cmdLen 0 poza 0x66/0x67, `Cmd_invalid`) -> odrzucenie lub ostrzezenie; (c) suma opoznien -> prawdziwa dlugosc;
   (d) brak 0x66 przed `dataEnd` -> ostrzezenie "ends early"; (e) liczba komend <= limit (np. 50 mln).
9. **Bloki danych 0x67** (w pre-skanie): `7 + dblkLen <= dataEnd - pozycja` (UINT64); `dblkLen` liczone po
   zamaskowaniu bitu 31; suma rozmiarow bankow PCM i suma `memSize` zapisow ROM <= budzet pamieci (np. 256 MiB
   [ZALOZENIE]); pojedynczy `memSize` <= limit (np. 64 MiB); dla typow skompresowanych (0x40..0x7E): `dblkLen >= 5`,
   typ kompresji znany (0/1), `decmpLen` <= limit, `bitsCmp` w `1..16`, `bitsDec` w `1..16`, `bitsCmp <= bitsDec`,
   tryb tabelowy: tabela 0x7F poprzedza, `valueCount >= 2^bitsCmp` (inaczej odczyt poza tabela, H3); typ 0x7F:
   `dblkLen >= 6` i `6 + valueCount*valSize <= dblkLen`. Zapisy RAM (0xC0+): `dblkLen >= 2/4`.
10. Blok 0x67 typow 0x00..0x3F PO komendzie `0x91` (Set Data) tego samego banku -> odrzucic lub ostrzec (wiszacy
    wskaznik DAC, H3); blok 0x67 po `loopOfs` -> ostrzezenie (H3, JAKOSC).
11. Naglowek chipow: zegary i liczba ukladow (maks. 2 instancje na typ; libvgm tak buduje `_devCfgs`); typ ukladu
    uzyty w strumieniu bez zegara w naglowku = ostrzezenie ("chip not present", libvgm po cichu ignoruje).
12. GD3: nasza wlasna kontrola dlugosci tagu (limit np. 4 KiB na pole), UTF-8 po stronie `format` (sanityzacja
    niesparowanych surogatow, usuniecie sterujacych/NUL) zanim trafi do ImGui; libvgm gubi tagi, gdy rejestr
    kodowan nie istnieje (H4) - w razie pustych tagow przy niepustym GD3 uzyc wlasnego dekodera UTF-16LE.
13. Granica wyjatkow i procesu: `try/catch (std::bad_alloc)` wokol `LoadFile/Start/Seek/Render`; pamiec liczona
    z gory przez pre-skan, aby `Resmpl_EnsureBuffers` (`abort()` przy OOM) nie wystapilo przy rozsadnych plikach.
14. Wersje: dla `fileVer < 0x150`/`0x110` libvgm stosuje obejscia (`:355-370`); pre-skan uzywa tych samych regul
    wersji, aby dlugosc `dataOfs` byla zgodna.
15. **Konfiguracja ukladow** (`format::parseVgmHeader`, odpowiedniki `GenerateDeviceConfig`): (a) C352 z `0xD6 == 0` ->
    `BadHeader` (libvgm dzieli przez bajt; nie mamy sposobu na odgadniecie dzielnika, wiec nie "naprawiamy" bufora);
    (b) SN76496 `0x2A > 32`, SegaPCM `0x3C > 31` -> `BadHeader` (przesuniecia o >= 32 bity to UB w rdzeniach);
    (c) zegar PO przeliczeniach libvgm liczony w UINT32 z zawinieciem tak jak libvgm (MultiPCM, K054539, C140/C219,
    QSound, SCSP, C352); wynik 0 -> `BadHeader`; (d) instancja ukladu z zerowym zegarem po zamaskowaniu bitow 30/31
    -> `BadHeader` (libvgm i tak by ja uruchomil - `GetChipCount` liczy po surowym polu).
16. **Gorne granice zegarow** (`Limits`, `LimitExceeded`): `maxChipClockHz` = 400 MHz (surowy i efektywny zegar),
    `maxChipSampleRate` = 20 MHz (natywny rate jednej instancji = zegar / najmniejszy dzielnik rdzenia z `emu/cores`,
    tabela `kRateDivisor` w `src/format/header.cpp`; 5x wiecej niz najszybszy realny uklad: POKEY/K005289 ~3,6 MHz),
    `maxTotalChipSampleRate` = 64 MHz (suma po instancjach => bufory `Resampler` <= ~512 MiB). 32X PWM (stale 22020 Hz)
    i ES5506 (rdzen jest szkieletem, nie startuje) nie licza sie do rate.
17. **Lacki bufora dla libvgm** (`format::applyLibvgmPatches`, wspolne dla loadera i `VgmDecoder::open`): `0x04` (eofOfs)
    := eof uzyty przez parser (`HeaderInfo::eofOffset`) - libvgm porownuje eofOfs z rozmiarem Z paddingiem, wiec pole 0
    albo wskazujace w padding dawaloby `dataEnd` w paddingu i inne `loopOfs`/GD3 niz pre-skan; `0x1C/0x20` := 0 przy
    `StreamReport::loopDisabled`. Obie poprawki dotycza tylko naszej kopii.
18. **Strumienie DAC (0x90-0x95)**: pre-skan symuluje stan strumieni wg `Cmd_DACCtrl_*` i `dac_control.c`: strumien
    powstaje przy kazdym 0x90 (id != 0xFF), ale 0x91-0x95 dzialaja dopiero po SKUTECZNYM 0x90 (uklad docelowy o typie
    < 48 istnieje w naglowku i jego rdzen startuje; `Running &= 0`); `bankID` jest zapisywany przez 0x91 nawet gdy
    ignorowane; `DataStep = (UINT8)(CmdSize * StepSize)` (CmdSize 2 dla SN76496-czestotliwosc, 32X PWM, QSound, K005289,
    BSMT2000; `StepSize` 0 -> 1 dopiero przez 0x91). Odrzucenie (`BadCommandStream`): 0x93 MSEC przy `freq == 0`,
    0x93 TOEND/BYTES przy `DataStep == 0`, 0x95 (zawsze tryb BYTES) przy `DataStep == 0` gdy bank wybrany i ID w banku.
    Komendy bezpiecznie ignorowane przez libvgm -> ostrzezenie `Other` (raz na rodzaj komendy). Stan NIE jest
    resetowany przy skoku petli, wiec petla jest skanowana drugi raz. Wskaznik na dane banku (pkt 10) liczony per
    strumien i tylko dla skutecznego 0x91; przestawienie strumienia zwalnia poprzedni bank.
19. Znaki kierunkowe/niewidoczne Unicode w GD3 (U+200B-200F, U+202A-202E, U+2066-2069) sa usuwane (`sanitized`).
Fuzzing: cele w `fuzz/` (libFuzzer, clang-cl): (1) `ValidateVgm(buf)` bez libvgm, (2) pelny `LoadFile+Start+Render(N)`
dla buforow, ktore przeszly walidacje, z ASAN. Dla stwierdzonych luk (H1-H3) test regresyjny powinien PADAC na
nieprzefiltrowanym wejsciu i przechodzic z walidatorem.

## 9. Opcje CMake libvgm (emu + player + utils, bez audio i programow)

Jest w korzennym `CMakeLists.txt` (szkielet): `BUILD_LIBAUDIO=OFF`, `BUILD_LIBEMU=ON`, `BUILD_LIBPLAYER=ON`,
`BUILD_TESTS=OFF`, `BUILD_PLAYER=OFF`, `BUILD_VGM2WAV=OFF`, `USE_SANITIZERS=OFF` (`libvgm/CMakeLists.txt:127-136`,
`:124`), `LIBRARY_TYPE=STATIC`, `REALLY_NEED_CXX14=OFF`, `SNDEMU__ALL=OFF` + `SNDEMU_<DEV>_ALL=ON` pojedynczo,
Ootake/AdLibEmu `OFF`, `UTIL_LOADERS=ON` (zlib), `UTIL_CHARSET_CONV=ON` z `UTIL_CHARCNV_WINAPI=ON`/ICONV OFF.
Uwagi z lektury (nie zmienialem korzenia):
- `UTIL_THREADING=ON` buduje `OSThread/OSMutex/OSSignal` i wymaga `find_package(Threads)` (`utils/CMakeLists.txt:37-57`),
  ale nic w `emu/` ani `player/` ich nie uzywa (grep: tylko `player.cpp` i `utils`) => bezpiecznie `OFF`
  [do potwierdzenia przy pierwszym buildzie]. Do rozwazenia przez autora szkieletu.
- `libvgm/CMakeLists.txt:15-17` ustawia `CMAKE_C_STANDARD 90`, `CMAKE_CXX_STANDARD 98` tylko dla swoich celow. `add_definitions(
  VGM_LITTLE_ENDIAN, HAVE_STDINT_H)` (`:44-55`) dotyczy tylko katalogu libvgm i NIE propaguje sie do naszego kodu; nasze
  naglowki (`vgmplayer.hpp`, `stdtype.h`) nie zawieraja kodu zaleznego od endianess, ale `stdtype.h:3` wybiera
  typy po `HAVE_STDINT_H` - dla zgodnosci zalecam dodac oba define do `vgmplay_engine` (`target_compile_definitions`)
  [ZALOZENIE: bez nich typy i tak sa tym samym typem na MSVC x64].
- Linkowanie: `vgm-player` linkuje `vgm-emu`/`vgm-utils` jako PRIVATE (`player/CMakeLists.txt:46`), wiec definicje
  `EC_*`/`SNDDEV_*` (PUBLIC na `vgm-emu`) nie przechodza tranzytywnie; `src/engine/CMakeLists.txt` linkuje wszystkie trzy
  (`vgm-player vgm-emu vgm-utils`) - poprawnie.
- `CMAKE_*_POSTFIX` (`libvgm/CMakeLists.txt:57-71`) zmienia tylko nazwy plikow bibliotek, nie nazwy celow.
- `StrUtils-CPConv_Win.c` uzywa `RegOpenKeyEx` => linkowanie z `advapi32` [do potwierdzenia na linkerze].
- Weryfikacja listy ukladow: test jednostkowy iteruje 48 wartosci `_DEV_LIST` (`vgmplayer.cpp:49-57`) i sprawdza
  `SndEmu_GetDevDecl(type, NULL, 0) != NULL` (patrz H6).

## 10. Interfejs C++ modulu `engine` (pseudo-kod, nie do `src/`)

```cpp
// ---- engine/include/engine/decoder.hpp ----
namespace vgmplay::engine {

struct TrackInfo {                        // metadane, dostepne po open(), bez Start()
  std::string format;                     // "VGM", w przyszlosci "S98"...
  uint32_t versionBcd;
  uint64_t introFrames, loopFrames;       // w probkach wyjscia dla biezacego sampleRate; loopFrames==0 -> brak petli
  uint64_t totalFrames;                   // z wlasnego skanu (pkt 8), nie z naglowka
  std::vector<std::pair<std::string,std::string>> tags;   // zsanityzowany UTF-8 (TITLE, GAME, ...)
  ChannelLayout layout;
  std::vector<std::string> warnings;      // z walidatora
};

struct DecoderConfig {                    // niezmienne po open()
  uint32_t sampleRate;                    // 44100/48000/...
  uint32_t maxBlockFrames;                // 512
  uint32_t loopCount = 2;                 // 0 = nieskonczony
  uint32_t fadeFrames;                    // 1 * sampleRate
  uint32_t endSilenceFrames = 0;
  CoreChoice cores;                       // per DevType+instancja -> coreId (0 = domyslny vgmplay)
};

struct RenderResult { uint32_t frames; bool ended; };   // ended: PLAYSTATE_FIN

class IDecoder {                          // TYLKO watek silnika; implementacje: VgmDecoder (libvgm), S98Decoder...
public:
  virtual ~IDecoder() = default;
  virtual Result<TrackInfo> open(std::span<const uint8_t> rawBuffer, const DecoderConfig&) = 0; // walidacja inside
  virtual Result<void>  start() = 0;                         // tworzy ukl., "dummy-render", zeruje mute
  virtual RenderResult  render(float* interleavedLR, uint32_t maxFrames, ScopeBlock* scope) = 0; // bez alokacji
  virtual Result<void>  seek(uint64_t frame) = 0;            // zob. "Seek i PcmRing"
  virtual void          fadeOut() = 0;
  virtual void          setChannelMute(DeviceId, uint32_t chnMask) = 0;   // chnMute (nie 'disable')
  virtual uint64_t      positionFrames() const = 0;          // pozycja RENDEROWANA (nie slyszana)
  virtual const ChipStateSnapshot& chipState() const = 0;    // zob. chipstate (E4)
  virtual std::vector<CoreInfo> availableCores(DevType) const = 0;
  virtual void          close() = 0;
};

struct DeviceDesc { DeviceId id; DevType type; uint8_t instance; std::string name;
                    uint16_t channelCount; std::vector<std::string> channelNames;
                    uint32_t coreFcc; bool hasLinked; };
struct ChannelLayout { std::vector<DeviceDesc> devices;
                       // kanaly "plaskie": (deviceIdx, channelIdx) -> globalChannelId
                       std::vector<ChannelRef> channels; };

// ---- ScopeBlock: jedna porcja do ScopeRing (POD, stala wielkosc, bez alokacji) ----
struct ScopeStreamDesc { uint16_t deviceIdx; uint16_t channelIdx; /*0xFFFF = suma ukladu*/ uint8_t flags; };
struct ScopeBlock {
  uint64_t epoch;                 // numer generacji (seek/load)
  uint64_t startFrame;            // pozycja pierwszej klatki w utworze
  uint32_t frames;                // <= maxBlockFrames
  float    masterL[MaxBlock], masterR[MaxBlock];            // E1: tylko to
  uint8_t  extraStreamCount;      // E4+: per uklad (wariant b), per kanal (a/c)
  // float extra[MaxExtra][MaxBlock];  - rezerwacja; pierwszy release: extraStreamCount = 0
};
```

```cpp
// ---- Engine (watek silnika + fasada dla UI) ----
class Engine {
public:
  Engine(const EngineConfig&, SpscQueue<Cmd>& cmds, SpscQueue<Event>& events,
         PcmRing& pcm, ScopeRing& scope, EngineAtomics& pos);
  // --- UI/inny watek (nie dotyka libvgm) ---
  bool post(const Cmd&);                       // Load{path|buffer}, Play, Pause, Stop, Seek{frame}, SetLoops, SetFade,
                                               // MuteChannel{dev,chn,bool}, SoloChannel{...}, SetCore{dev,instance,fcc},
                                               // SetSampleRate, SetTapMode{...}; false = kolejka pelna
  bool pollEvent(Event& out);                  // TrackLoaded{TrackInfo}, TrackEnded, Error{code,text}, Warning, SeekDone,
                                               // LoopEntered{n}, DeviceChanged, Underrun
  PositionSnapshot snapshot() const;           // z atomics: renderedFrame, audibleFrame, state, loopIndex, fadeActive
  // --- watek silnika ---
  void run();                                  // petla: drain cmds -> render blok -> push PcmRing/ScopeRing/chipstate
private:
  std::unique_ptr<IDecoder> dec_;              // VgmDecoder (PlayerA + TapVgmPlayer)
  // nie wolno trzymac wskaznikow do danych libvgm (tagi, DEV_INFO) poza open(): kopiowac od razu
};
```

Zasady i decyzje towarzyszace:
- **Watki.** `IDecoder` i libvgm tylko z watku silnika; UI widzi wylacznie `Cmd/Event/snapshot`, skopiowane `TrackInfo`.
  Brak blokad w `render`. Callbacki libvgm (zdarzenie/log) tylko wpisuja do wewnetrznych POD-struktur, ktore
  petla silnika zamienia na `Event`.
- **Seek i PcmRing.** Kazdy blok w `PcmRing` niesie `epoch`; `Seek`/`Load` zwieksza `epoch` (atomik); callback
  odrzuca (przesuwa wskaznik odczytu, bez kopiowania) bloki z nieaktualnym `epoch`. Mikro-fade: pierwszy blok nowego
  `epoch` ma narastanie ~5 ms robione w silniku; ucinanie starej tresci wymaga krotkiego opadania ~2 ms w
  callbacku (kilkadziesiat mnozen) - alternatywa: trzymac `PcmRing` ~2 bloki (<= 25 ms). Decyzja do ADR 0003
  (`PcmRing`/`audio_out`); poza zakresem tego dokumentu.
- **Pozycja.** Pozycja slyszana = `startFrame` bloku + klatki zuzyte przez callback (licznik w callbacku), nie
  `GetCurPos` (to pozycja renderowana, wyprzedza o zawartosc pierscienia).
- **Load.** Na watku silnika: walidacja (pkt 8) -> `open` -> `start` (+ "dummy-render") -> zdarzenie `TrackLoaded`.
  `ScopeBlock`/`ChannelLayout` sa gotowe na per-uklad i per-kanal bez zmiany ABI.
- **Eksport WAV (pozniej).** Ten sam `IDecoder::render` wolany w petli offline (bez ringow), `PlayerA` daje
  `GetTotalTime`; bez zmian w interfejsie.
- **Inne formaty.** `VgmDecoder` jest jedyna implementacja w pierwszym wydaniu; S98 = rejestracja `S98Player` w
  `PlayerA` (libvgm), NSF/SPC = nowe implementacje `IDecoder` z innych bibliotek (do osobnego ADR; libvgm ich nie ma).

## Konsekwencje i ryzyka

- Podklasa `TapVgmPlayer` uzaleznia nas od wnetrzosci przypietego commitu libvgm; mitigacja: przypiecie,
  test rownowaznosci wyjscia (podklasa vs `VGMPlayer` bit w bit) na korpusie, kompilacja z `static_assert` na
  rozmiary struktur gdzie sie da.
- Walidator (pkt 8) jest wiekszym kawalkiem pracy niz "sprawdz naglowek"; jest warunkiem uzywania libvgm na
  niezaufanych plikach.
- Wydajnosc seeka i koszt (c) - tylko szacunki; wymagaja benchmarku przed decyzja.
- Dopuszczalnosc lat (a) zalezy od decyzji licencyjnej w `docs/licensing/libvgm-audit.md` (brak LICENSE w upstream).

## Niesprawdzone zalozenia (do weryfikacji po pierwszym buildzie)

1. Kompilacja podklasy `VGMPlayer` w C++20 z naglowkow pisanych pod C++98 (dostep do `protected`, `_CMD_INFO`, nadpisanie `Render`).
2. `UTIL_THREADING=OFF` nie psuje budowy `vgm-player`/`vgm-emu`.
3. Nazwa celu i linkowanie `advapi32` dla `CPConv_Win`.
4. Wszystkie 48 ukladow z `_DEV_LIST` maja deklaracje w naszym zestawie wlaczonych urzadzen (H6).
5. Ktore rdzenie maja `SetMuteMask == NULL` oraz zachowanie `write_rom` w rdzeniach poza okim6295/c352.
6. Wydajnosc: seek (rzad ms vs ~1 s dla PCM-owych), koszt (c), koszt wariantu (b).
7. Limity (64 MiB, 256 MiB budzet, 4 KiB tag) to propozycje, nie wynik pomiaru korpusu.
8. Interpretacja "wyciszenie na koncu 1 s" dla plikow bez petli (ogon ciszy vs fade).
