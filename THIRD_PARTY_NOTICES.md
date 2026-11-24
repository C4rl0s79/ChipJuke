# Składniki zewnętrzne

vgmplay jest dystrybuowany na licencji GPL-2.0-only (zob. `LICENSE`). Poniżej składniki innych autorów.
Teksty licencji składników dołączane do paczki ZIP są w folderze `licenses/` (pliki `*-LICENSE.txt` skopiowano
z `vcpkg_installed/x64-windows-static/share/<pakiet>/copyright` dla wersji z `vcpkg.json` + `builtin-baseline`;
po aktualizacji zależności trzeba je odświeżyć).
Wszystkie biblioteki są linkowane statycznie do `vgmplay.exe` (triplet `x64-windows-static`, statyczne CRT),
więc paczka nie zawiera żadnych DLL.

**Kod źródłowy odpowiadający binarce (GPL-2.0)**: repozytorium i commit są zapisane w pliku `SOURCE.txt` w paczce
(adres repozytorium: `<REPOSITORY-URL: uzupełnić przy publikacji>`).

| Składnik | Licencja | Uwagi |
|---|---|---|
| libvgm (Valley Bell), submoduł `external/libvgm`, commit c5c03a2c2514d38d2ab7b7e8dbd021af0b8292cb | mieszana (rdzenie emulacji: BSD-3-Clause, GPL-2.0+, LGPL-2.1+, MIT i inne; kod własny libvgm bez pliku LICENSE) | Szczegóły, lista rdzeni wyłączonych z budowania i ryzyka: [docs/licensing/libvgm-audit.md](docs/licensing/libvgm-audit.md). Zbudowany plik wykonywalny, który zawiera kod GPL, musi być udostępniany z kodem źródłowym. |
| SDL3 (vcpkg 3.4.20) | zlib | `licenses/SDL3-LICENSE.txt`;  https://libsdl.org |
| Dear ImGui (vcpkg 1.93.0) | MIT | `licenses/DearImGui-LICENSE.txt`;  https://github.com/ocornut/imgui |
| miniaudio (vcpkg 0.11.26) | MIT-0 lub domena publiczna (do wyboru) | `licenses/miniaudio-LICENSE.txt`;  https://miniaud.io |
| zlib (vcpkg 1.3.2) | zlib | `licenses/zlib-LICENSE.txt`;  https://zlib.net |
| nlohmann/json (vcpkg 3.12.0) | MIT | `licenses/nlohmann-json-LICENSE.txt`;  https://github.com/nlohmann/json |
| doctest (tylko testy, nie wchodzi do paczki) | MIT | https://github.com/doctest/doctest |
| BIZ UDGothic Regular (font osadzony w .exe jako zasób) | SIL OFL 1.1 | https://github.com/googlefonts/morisawa-biz-ud-gothic — tekst licencji: `assets/fonts/OFL.txt`, w paczce `licenses/BIZUDGothic-OFL.txt` |

## Wykorzystane rdzenie emulacji libvgm (skrót)

Pochodzenie kodu w libvgm: MAME (BSD-3-Clause lub GPL-2.0+ wg nagłówków plików), Nuked OPL/OPLL/OPN2/OPM
(Nuke.YKT; LGPL-2.1+ lub GPL-2.0+), emu2413 i emu2149 (Mitsutaka Okazaki; MIT), Gens (GPL-2.0+),
Mednafen VSU (GPL-2.0+), NSFPlay (licencja do wyjaśnienia), SameBoy (MIT), laoo Mikey (MIT).
Pełna tabela per plik: `docs/licensing/libvgm-audit.md`.

## Uwaga o licencji libvgm (przed publikacją binarek)

Repozytorium libvgm nie zawiera pliku LICENSE ani COPYING, a własny kod Valley Bell nie ma nagłówków licencji
(upstream #129, #43, PR #141). W domyślnej konfiguracji (`VGMPLAY_LIBVGM_STRICT_LICENSES=OFF`, decyzja właściciela
projektu) włączone są też rdzenie bez nagłówka licencji, mające zamiennik (NSFPlay, Maxim SN76489, Gens YM2612/SCD PCM,
superctr QSound). Rdzenie z klauzulą niekomercyjną (`Ootake_PSG`) i `adlibemu` są wyłączone. Szczegóły i lista
rdzeni: [docs/licensing/libvgm-audit.md](docs/licensing/libvgm-audit.md) (w paczce: `licenses/libvgm-audit.md`).
Rdzenie BSD-3-Clause i MIT wymagają zachowania not praw autorskich — pozostają one w nagłówkach plików źródłowych
(dostępnych razem z kodem; autorzy per plik w audycie).

## Pliki w paczce

`licenses/`: `SDL3-LICENSE.txt`, `DearImGui-LICENSE.txt`, `miniaudio-LICENSE.txt`, `zlib-LICENSE.txt`,
`nlohmann-json-LICENSE.txt`, `BIZUDGothic-OFL.txt`, `libvgm-audit.md`. Pełny tekst GPL-2.0: `LICENSE`.
doctest (MIT) służy tylko do testów i nie wchodzi do paczki.
