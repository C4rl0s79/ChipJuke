# libvgm (c5c03a2): luki walidacji parsera VGM - dowody z kodu

Status: wynik CZYTANIA kodu (bez kompilacji, bez uruchomienia, bez ASAN). Kazde "mozliwe" znaczy: wynika z
lektury, nie zostalo odtworzone. Sciezki wzgledem `external/libvgm/`. Wnioski sluza liscie walidacji w
`docs/adr/0002-libvgm-integration.md` (pkt 8) oraz celom fuzzingu.

## H1. Naglowek: odczyt poza buforem przy malym pliku
- `player/vgmplayer.cpp:253` - jedyne sprawdzenia rozmiaru: `size >= 0x38` i sygnatura "Vgm ".
- `:283-291` - `dataOfs` z naglowka (v>=1.50) albo 0x40; wartosc < 0x38 jest podnoszona do 0x38, GORNY limit
  (<= rozmiar pliku) nie jest sprawdzany.
- `:291-300` - `_hdrLenFile = dataOfs` (ucinane do 0x100), potem `memcpy(_hdrBuffer, _fileData, _hdrLenFile)`.
  Gdy plik ma np. 0x38..0x3F bajtow (dataOfs domyslnie 0x40) albo dataOfs > rozmiar, memcpy czyta poza
  alokacja `DataLoader` (realloc o dokladnie `bytesTotal`, `utils/DataLoader.c:105`). Odczyt, nie zapis.
- `:293` odczyt `_fileData[0xBC..0xBF]` gdy `_hdrLenFile >= 0xC0` - jw. (rozmiar pliku nie jest sprawdzany).
- `:331-337` - `extraHdrOfs < eofOfs`, ale odczyty `ReadLE32(&_fileData[extraHdrOfs])` i `+4`, `+8`
  moga siegnac do 11 bajtow za koniec (extraHdrOfs bliski eof).
- `ReadRelOfs` (`:121-126`) - `fileOfs + ofs` w UINT32 z zawinieciem; po zawinieciu offset wskazuje do tylu.
  Konsumenci maja wlasne sprawdzenia `< size` (`:382`, `:408`), wiec to raczej bledne dane niz crash.
- `eofOfs` (`:319-324`) jest poprawiany do rozmiaru pliku, `dataEnd = eofOfs`, GD3 w srodku danych skraca
  `dataEnd` (`:328`). `loopOfs` musi lezec w `[dataOfs, dataEnd)` (`:342`), `loopTicks==0` => brak petli (`:347`).

## H2. Strumien komend: operandy ostatniej komendy za `dataEnd`
- `ParseFile` (`:2095-2101`) sprawdza tylko `_filePos < dataEnd` przed wywolaniem handlera; handlery czytaja
  `fData[0x01..]` (do 0x0B bajtow, tabela `_CMD_INFO`, `player/vgmplayer_cmdhandler.cpp:20-279`).
  Urwana ostatnia komenda => odczyt do ~11 bajtow poza buforem. To samo w `SeekToFilePos` (`:2012-2017`),
  `ParseFileForFMClocks` (`:2139-2170`) i `ParseFileForOPL4ROMRequirement` (ta akurat sprawdza `cmdLen`,
  `:1855`, `:1919`).

## H3. Data block (0x67) - brak sprawdzenia dlugosci wzgledem pliku
- `Cmd_DataBlock` (`player/vgmplayer_cmdhandler.cpp:681-810`): `dblkLen` (31 bit) czytane z pliku (`:694-696`).
  NIGDZIE nie jest porownywane z `dataEnd - filePos` (porownaj poprawny wzorzec w
  `ParseFileForOPL4ROMRequirement`, `player/vgmplayer.cpp:1833`).
- Typ 0x00..0x7F (bank PCM): `pcmBnk->data.resize(oldLen + dataLen)` (`:730`) i `memcpy(..., dataPtr, dataLen)`
  (`:744`) - odczyt poza buforem pliku do ~2 GiB; `oldLen` jest `UINT32` (`:714`), a `oldLen + dataLen`
  moze sie zawinac => `resize` mniejsze niz kopiowane dane (zapis poza buforem, mozliwe przy kilku blokach).
  Alokacja do 2 GiB na blok; `std::bad_alloc` z watku silnika nie jest nigdzie lapany.
- Typ 0x40..0x7E (skompresowany): wynik `ReadComprDataBlkHdr` jest IGNOROWANY (`:720`). Przy `dblkLen < 5`
  (zwraca 0x10) lub nieznanym typie (0x80) struktura `dbCI` zostaje niezainicjalizowana, a
  `dataLen = dbCI.decmpLen` (`:722`) oraz `dblkLen - dbCI.hdrSize` (`:734`) uzywaja smieci z stosu.
  `decmpLen` (do 4 GiB) pochodzi z pliku.
- `player/dblk_compr.c:158`, `:239`, `:320`, `:377` - `MUL_DIV(inLen, 8|16, bitsCmp)` dzieli przez `bitsCmp`
  z pliku; `bitsCmp == 0` => dzielenie przez zero (SIGFPE/wyjatek SEH) w watku silnika.
- Tryb tabelowy: `ent1B[inVal]` / `ent2B[inVal]` (`:188`, `:271`), `inVal` do `2^bitsCmp-1`, a tabela ma
  `valueCount` wpisow (sprawdzane jest tylko `!= 0` i zgodnosc bitsDec/bitsCmp, `:145-153`) => odczyt poza
  tabela. `ReadPCMComprTable` (`:727-769`) przy `bitsDec == 0` daje `valSize == 0`.
- Typ 0x7F (tabela): `ReadPCMComprTable(dblkLen, &fData[0], ...)` (`cmdhandler:708`) ufa `dblkLen`.
- Typ 0x80..0xBF (zapis ROM): `memSize = ReadLE32` (`:758`) trafia do `romSize` => rdzenie robia
  `realloc(memSize)` + `memset` (np. `emu/cores/okim6295.c:618-630`, `emu/cores/c352.c:456-469`), bez
  sprawdzenia NULL (okim6295:625-627) => do 4 GiB na blok, mozliwy NULL-deref przy braku pamieci.
  `write_rom` w tych dwoch rdzeniach przycina `offset/length` do rozmiaru, ale `offset + length` jest w
  UINT32 (c352.c:475) - bezpieczne dopiero przy ograniczonym `dblkLen`. Przejrzano TYLKO okim6295 i c352;
  pozostale rdzenie ROM-owe NIE byly audytowane.
- Typ 0xC0..0xFF (zapis RAM): `dataLen = dblkLen - 2/4` z sprawdzeniem `dblkLen >= 2/4` (`:787`, `:796`);
  `romWrite(dataOfs, dataLen, dataPtr)` bez weryfikacji, ze `dataLen` miesci sie w pliku (patrz wyzej).
- JAKOSC (nie bezpieczenstwo): `return` w `:704`, `:728`, `:787`, `:797` wychodzi PRZED `_filePos += dblkLen`
  (`:808`). Handler ma `cmdLen == 0` (`:126`), wiec `_filePos` zostaje na poczatku payloadu, ktory jest potem
  wykonywany jako komendy. Dotyczy: bloku 0x67 po `loopOfs` w 2. i dalszych petlach (`_curLoop > 0`), oraz
  zbyt krotkich blokow RAM. Normalne VGM maja bloki przed petla, wiec zwykle nieszkodliwe.
- Dangling pointer (znany TODO, `:747`): `daccontrol_set_data` (`:931-933`) przechowuje wskaznik do
  `pcmBnk->data[0]`; kolejny blok danych do tego samego banku robi `resize` (realokacja) i wskaznik w strumieniu
  DAC wisi. Odczyt w `emu/dac_control.c:106`. Mozliwy use-after-free dla plikow, w ktorych blok 0x67 pojawia sie
  po komendzie 0x91 dla tego banku.

## H4. GD3
- `LoadTags` (`player/vgmplayer.cpp:432-482`): `gd3Ofs < eofOfs`, `gd3Ofs + 0xC <= eofOfs`, sygnatura "Gd3 ",
  wersja `0x100..0x1FF`, `eotPos` przycinany do `eofOfs` (`:459`). `eotPos = len + curPos` moze sie zawinac
  (`:456-458`) - skutek: petla konczy sie wczesnie (bezpieczne).
- Odczyt UTF-16 (`:470`) czyta 2 bajty przy `curPos < eotPos`; przy nieparzystej reszcie 1 bajt za `eofOfs`.
- Wynik `LoadTags` jest ignorowany przez `LoadFile` (`:270`); zly GD3 => puste tagi, nie blad.
- Konwersja UTF-16LE->UTF-8 przez WinAPI (`utils/StrUtils-CPConv_Win.c`): kodowanie jest wyszukiwane w
  REJESTRZE (`HKLM\SOFTWARE\Classes\MIME\Database\Charset`, `:25`, `:43-95`). Gdy zawiedzie, `_cpcUTF16 == NULL`
  (`vgmplayer.cpp:201-203`) i WSZYSTKIE tagi sa puste (`:486`). Dlugosc tagu ograniczona tylko rozmiarem pliku;
  nie ma gwarancji poprawnego UTF-8 (niesparowane surogaty) - sanityzowac przed ImGui.
- Konstruktor `VGMPlayer` robi `CPConv_Init` (rejestr) - budowac raz, nie per utwor.

## H5. Gzip w MemoryLoader
- `utils/MemoryLoader.c:58` - wykrycie po 2 bajtach (0x1F 0x8B) i `srcSize >= 18`; `decSize` = ostatnie 4 bajty
  pliku (ISIZE, `:66`) - dane niezaufane. `DataLoader_Load` ustawia `_bytesTotal = dlength = decSize`
  (`DataLoader.c:63`) a `DataLoader_Read` robi `realloc(_data, _bytesTotal)` (`:105`) PRZED inflate. Brak
  limitu: ISIZE do 4 GiB-1 => zadanie alokacji do 4 GiB niezaleznie od realnych danych. Przy ISIZE mniejszym
  niz prawdziwa dlugosc dane sa ciche przycinane (`MemoryLoader.c:84-85`).
- `MemoryLoader_ReadDataGZ` (`:97-114`) ignoruje kody bledow inflate (komentarz TODO `:105-108`): uciety/uszkodzony
  gzip daje "udany" odczyt z czesciowymi danymi.
- `DataLoader_Read`: `realloc` bez zachowania starego wskaznika (`:105-108`) => wyciek przy OOM (drobne).
- Wniosek: dekompresja gzip NALEZY do nas (zlib, limit rozmiaru, kontrola bledow); do libvgm przekazujemy
  bufor surowy. (Surowy bufor zaczynajacy sie od 1F 8B nie moze byc poprawnym VGM - "Vgm " to bajty 56 67 6D 20.)

## H6. Inne
- `Resmpl_EnsureBuffers` (`emu/Resampler.c:17-31`) wola `abort()` przy braku pamieci => zabija caly proces.
- `GetSongDeviceInfo` (`vgmplayer.cpp:575-576`, `:630-635`) dereferencuje `devDecl` z `SndEmu_GetDevDecl` bez
  sprawdzenia NULL. NULL, gdy uklad wystepuje w pliku, a rdzen nie jest skompilowany (SNDDEV_SELECT). Przy
  naszej liscie wlaczonych urzadzen (root `CMakeLists.txt`) kazdy uklad z `_DEV_LIST` ma deklaracje - ZALOZENIE do
  sprawdzenia testem na zbudowanej bibliotece (wszystkie 48 wartosci `_DEV_LIST`).
- Naglowkowe `numTicks`/`loopTicks`/`loopOfs` sa niezaufane i nie musza zgadzac sie z faktyczna dlugoscia
  strumienia; rzeczywisty koniec wyznacza `0x66`. Do UI warto liczyc dlugosc z wlasnego skanu.
- Domyslne rdzenie wpisane na stale w `vgmplayer.cpp`: SN76496=MAME (`:1501`), RF5C68=MAME, RF5C164=GENS
  (`:1508-1515`), QSound=CTR (`:1575`), polaczone AY8910=EMU2149 i YMF262=AdLibEmu (`:1766-1769`). Wylaczenie
  rdzenia w CMake (Gens/ctr/adlibemu) BEZ nadpisania `emuCore` w opcjach urzadzenia oznacza `EERR_NOT_FOUND`
  (`emu/SoundEmu.c:383`, `:392`) i brak dzwieku danego ukladu (dla linkowanego: `helper.c:39-45`).

## H7. Konfiguracja ukladow z bajtow naglowka (`GenerateDeviceConfig`, vgmplayer.cpp:1253-1426) - dopisane po recenzji
Status: odczyt kodu + potwierdzenie crashu na buildzie wydaniowym (reprodukcje w `tests/data/fuzz-crashes/`).
- C352 (`:1373-1376`): `devCfg.clock = devCfg.clock * 72 / _hdrBuffer[0xD6]` - dzielenie przez bajt z pliku bez ochrony
  (#DE przy `LoadFile`). Obrona: `BadHeader` gdy 0xD6 == 0 (patrz ADR pkt 15).
- Mnozenia zegarow w UINT32 z zawinieciem: MultiPCM `*224/180` (`:1321`), C352 `*72`, K054539 `*384` (`:1338`), C140/C219
  `*576`, QSound `*15`, SCSP `*512` (wszystkie tylko dla zegarow < 1-5 MHz, wiec bez zawiniecia; zawija MultiPCM i C352).
  Wynik 0 (np. MultiPCM 2^27, C352 2^29 przy 0xD6 = 72) daje zegar 0 w rdzeniu. Sprawdzamy zegar efektywny.
- SN76496 (`:1278`): `shiftRegWidth` = bajt 0x2A trafia do `1 << (width - 1)` (`sn76496.c:665`, `sn76489.c:369`) i petli
  `1 << i` (`sn76496.c:652`) - UB dla > 32 (na x86 przesuniecie mod 32, bez crasha). SegaPCM (`:1298`): `bnkshift` = 0x3C w
  `0x1fffff >> shift` (`segapcm.c:211`) - to samo. Odrzucane (ADR pkt 15b).
- AY8910 (`chipType`/`chipFlags`), MSM6258 (`& 3` -> indeks tablicy 4 wpisow), MSM5205 (`& 3`), ES5503 (`flags` = liczba
  kanalow, `pow2_mask`), K054539/C140 (`flags` = typ bankowania, `switch` z domyslna galezia) - bez dzielen i indeksowania
  poza tablica; przejrzane, bez zmian.
- ES5506 ma w libvgm tylko szkielet (`emu/cores/es5506.c`, brak rdzenia): `SndEmu_Start2` zawodzi i uklad jest po cichu
  pomijany (`InitDevices`, `retVal`), wiec nie istnieje jako cel DAC.
- Instancja z zerowym zegarem: `GetChipCount` liczy po surowym polu (bit 30 -> 2), a `GenerateDeviceConfig` tworzy
  konfiguracje takze dla zegara 0 po zamaskowaniu. Dawne "zegar 0 -> pomin i ostrzez" rozjezdzalo nasza liste ukladow z
  libvgm; teraz `BadHeader`.
- NIEOBSLUZONE (ryzyko resztkowe, tylko odczyt): `segapcm.c:177` czyta `rom[offset | (addr >> 8)]` bez porownania z
  `ROMSize`; indeks moze siegnac 0x1FFFFF, a ROM ma domyslnie 0x80000 B (albo `memSize` bloku ROM). Zalezy od naglowka
  (bnkshift/bnkmask) i od rejestrow zapisanych komendami 0xC0 - wymaga emulacji rejestrow SegaPCM w pre-skanie.
  Podobnie `daccontrol_SendCommand` czyta `ChipData[1]` dla 2-bajtowych komend o 1 bajt za koniec banku.

## H8. DAC Stream Control (0x90-0x95): dzielenie przez zero
- `Cmd_DACCtrl_Setup` (`cmdhandler:871-915`): strumien trafia do `_dacStrmMap` ZANIM sprawdzony jest uklad docelowy;
  `GetDevicePtr == NULL` (typ >= 48, brak zegara, rdzen nie wystartowal) konczy komende bez `daccontrol_setup_chip`.
- `daccontrol_*` (`dac_control.c:405-515`) wracaja natychmiast, gdy `Running & 0x80` (init `0xFF`; zdejmuje je dopiero
  skuteczne setup). Dlatego 0x91/0x92/0x93 sa ignorowane przed skutecznym 0x90, ale `bankID` (0x91) i `lastItem` sa
  zapisywane mimo to.
- `daccontrol_start` (`:516-560`): MSEC `1000 * Length / Frequency`, TOEND `.../ DataStep`, BYTES `Length / DataStep` -
  `Frequency` (0 do pierwszego 0x92) i `DataStep` (UINT8 = CmdSize * StepSize; StepSize 0 do pierwszego 0x91; 2 * 128
  zawija sie do 0) moga byc zerem. Potwierdzone: 0x93 MSEC (freq 0), TOEND, BYTES oraz 0x95 (zawsze BYTES) z DataStep 0
  koncza proces kodem 0xC0000094; 0x93/0x95 bez skutecznego setupu lub 0x95 bez wybranego banku sa bezpieczne.
- Stan strumieni przezywa skok petli (`Cmd_EndOfData` zmienia tylko `_filePos`; reset tylko w `VGMPlayer::Reset`), wiec
  `0x92 freq=0` na koncu petli psuje 0x93 MSEC na jej poczatku w 2. przebiegu.
- Pamiec: wskaznik `Data` w strumieniu ustawia wylacznie skuteczne 0x91 (`daccontrol_set_data`); pozniejszy blok PCM w tym
  samym banku realokuje wektor (dangling, H3).

## H9. Zegar bez gornej granicy i bufory resamplera; eofOfs a padding
- `Resmpl_Init` (`Resampler.c:103`) `Resmpl_EnsureBuffers(rate)` -> `malloc(rate * 2 * sizeof(DEV_SMPL))`, `abort()` przy
  NULL (`:26-27`). Rate = zegar / dzielnik rdzenia (tabela w `src/format/header.cpp`); najwyzsze: POKEY, K005289, MSM5205
  w trybie slave (rate = zegar), SN76496 (zegar / 2). Zegar POKEY 0x3FFFFFFF => ~8 GiB => `abort()` (reprodukcja
  `known_pokey_huge_clock.vgm`). Obrona: `Limits::maxChipClockHz`, `maxChipSampleRate`, `maxTotalChipSampleRate`.
- `LoadFile` (`vgmplayer.cpp:322-327`) porownuje `eofOfs` z `DataLoader_GetSize` czyli rozmiarem Z paddingiem (>= +0x110 B).
  Pole 0, wskazujace w padding albo za niego daje `dataEnd` w paddingu (zera = komendy 0x00 "unknown") lub rozmiar z
  paddingiem; `loopOfs` w paddingu przechodzi test `< dataEnd`. Obrona: `applyLibvgmPatches` zapisuje w kopii eof parsera.
- Blok skompresowany z `decmpLen == 0` i `dblkLen == 10` (sam naglowek): `return` w `cmdhandler:727` przed przeskoczeniem
  payloadu - 10 bajtow naglowka wykonane jako komendy; bajt `bitsDec` (1..16) to komenda "invalid", wiec odtwarzanie sie
  urywa (nie crash). Pre-skan odrzuca tylko `dblkLen > 10`; zostawione.
- Pusty blok (`dblkLen == 0`) w/za `loopOfs`: `_filePos += 7` jest PRZED `return` w `_curLoop > 0`, wiec nie ma payloadu do
  wykonania - nie wylacza petli.
