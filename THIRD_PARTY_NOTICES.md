# Składniki zewnętrzne

vgmplay jest dystrybuowany na licencji GPL-2.0-only (zob. `LICENSE`). Poniżej składniki innych autorów.
Wersje i dokładne licencje należy potwierdzić po pierwszym zbudowaniu (vcpkg zapisuje je w
`vcpkg_installed/<triplet>/share/<pakiet>/copyright`).

| Składnik | Licencja | Uwagi |
|---|---|---|
| libvgm (Valley Bell), submoduł `external/libvgm`, commit c5c03a2c2514d38d2ab7b7e8dbd021af0b8292cb | mieszana (rdzenie emulacji: BSD-3-Clause, GPL-2.0+, LGPL-2.1+, MIT i inne; kod własny libvgm bez pliku LICENSE) | Szczegóły, lista rdzeni wyłączonych z budowania i ryzyka: [docs/licensing/libvgm-audit.md](docs/licensing/libvgm-audit.md). Zbudowany plik wykonywalny, który zawiera kod GPL, musi być udostępniany z kodem źródłowym. |
| SDL3 | zlib | https://libsdl.org |
| Dear ImGui | MIT | https://github.com/ocornut/imgui |
| miniaudio | MIT-0 lub domena publiczna (do wyboru) | https://miniaud.io |
| zlib | zlib | https://zlib.net |
| nlohmann/json | MIT | https://github.com/nlohmann/json |
| doctest (tylko testy, nie wchodzi do paczki) | MIT | https://github.com/doctest/doctest |
| BIZ UDGothic Regular (font osadzony w .exe jako zasób) | SIL OFL 1.1 | https://github.com/googlefonts/morisawa-biz-ud-gothic — tekst licencji: `assets/fonts/OFL.txt`, w paczce `licenses/BIZUDGothic-OFL.txt` |

## Wykorzystane rdzenie emulacji libvgm (skrót)

Pochodzenie kodu w libvgm: MAME (BSD-3-Clause lub GPL-2.0+ wg nagłówków plików), Nuked OPL/OPLL/OPN2/OPM
(Nuke.YKT; LGPL-2.1+ lub GPL-2.0+), emu2413 i emu2149 (Mitsutaka Okazaki; MIT), Gens (GPL-2.0+),
Mednafen VSU (GPL-2.0+), NSFPlay (licencja do wyjaśnienia), SameBoy (MIT), laoo Mikey (MIT).
Pełna tabela per plik: `docs/licensing/libvgm-audit.md`.

Teksty licencji składników dołączane do paczki ZIP (`licenses/`) — TODO przed pierwszym wydaniem.
