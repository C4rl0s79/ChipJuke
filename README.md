# vgmplay

Odtwarzacz plików VGM/VGMZ (VGZ) w stylu trackera dla Windows. Emulacja układów dźwiękowych
opiera się na bibliotece [libvgm](https://github.com/ValleyBell/libvgm), interfejs na SDL3
i Dear ImGui (Direct3D 11), wyjście audio na miniaudio.

Status: wczesny etap (E0) — szkielet repozytorium, brak jeszcze działającej aplikacji.

## Wymagania

- Windows 10 22H2 lub nowszy (x64) — minimalny system dla aplikacji.
- Visual Studio 2022 z obciążeniem „Programowanie aplikacji klasycznych w C++” (MSVC x64, CMake, Ninja).
- [vcpkg](https://github.com/microsoft/vcpkg) i zmienna środowiskowa `VCPKG_ROOT` wskazująca jego katalog.
- Git (z obsługą submodułów).

## Budowanie

```powershell
git clone --recurse-submodules <adres-repozytorium> vgmplay
cd vgmplay
# Jeśli sklonowano bez --recurse-submodules:
git submodule update --init --recursive
```

Uruchom „x64 Native Tools Command Prompt for VS” (albo „Developer PowerShell for VS”), żeby `cl` i `ninja`
były w `PATH`, następnie:

```powershell
cmake --preset msvc-x64-release
cmake --build --preset msvc-x64-release
ctest --preset msvc-x64-release
```

Dostępne presety: `msvc-x64-debug`, `msvc-x64-release`, `clang-cl-asan-fuzz` (ASan + libFuzzer).
Katalog budowania: `out/build/<preset>`. Visual Studio otwiera presety bezpośrednio
(Plik → Otwórz → Folder).

Zależności (SDL3, Dear ImGui, miniaudio, zlib, nlohmann-json, doctest) instaluje vcpkg w trybie manifestu
(`vcpkg.json`) przy pierwszym konfigurowaniu; używany jest triplet `x64-windows-static`.

TODO: dopisać `builtin-baseline` do `vcpkg.json` (przypięcie wersji pakietów) po wybraniu commita vcpkg.

Opcje CMake projektu: `VGMPLAY_BUILD_TESTS`, `VGMPLAY_BUILD_FUZZ`, `VGMPLAY_STATIC_CRT`,
`VGMPLAY_WARNINGS_AS_ERRORS`.

## Dystrybucja

Wyłącznie przenośny ZIP (bez instalatora). Pakiet powstaje przez `cmake --install` (zob. `.github/workflows/ci.yml`).

## Licencja

Aplikacja: **GPL-2.0-only** — pełny tekst w pliku [LICENSE](LICENSE).
Licencje składników zewnętrznych: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Audyt licencji libvgm (wpływa na wybór rdzeni emulacji): [docs/licensing/libvgm-audit.md](docs/licensing/libvgm-audit.md).
