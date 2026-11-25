cmake_minimum_required(VERSION 3.25)

# Biblioteki vcpkg linkowane statycznie => statyczne CRT (jeden przenosny plik exe).
# Musi byc zgodne z tripletem vcpkg (x64-windows-static); patrz CMakePresets.json.
option(VGMPLAY_STATIC_CRT "Statyczne CRT (/MT, /MTd); wymaga tripletu x64-windows-static" ON)
if(VGMPLAY_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
# libvgm deklaruje cmake_minimum_required(3.12), wiec polityki nowsze niz 3.12
# dziedziczy z tej zmiennej. Bez niej CMAKE_MSVC_RUNTIME_LIBRARY bylaby tam ignorowana
# i libvgm zostalby zbudowany z /MD (konflikt CRT przy linkowaniu).
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# Jedyne zrodlo prawdy dla wersji: VERSION ponizej + VGMPLAY_VERSION_SUFFIX (pre-release, np. "rc1";
# pusty = wydanie finalne). Reszta (version.hpp, VERSIONINFO, nazwa paczki) jest z nich wyprowadzana.
# vcpkg.json ("version-string") musi byc zgodny - sprawdza to sekcja "Wersja" nizej.
project(vgmplay VERSION 1.0.0 LANGUAGES C CXX)
set(VGMPLAY_VERSION_SUFFIX "rc3")

if(PROJECT_IS_TOP_LEVEL)
    set(_vgmplay_tests_default ON)
else()
    set(_vgmplay_tests_default OFF)
endif()
option(VGMPLAY_BUILD_TESTS "Buduj testy jednostkowe/integracyjne (doctest)" ${_vgmplay_tests_default})
option(VGMPLAY_BUILD_FUZZ  "Buduj cele fuzzingu (libFuzzer; clang-cl)" OFF)
option(VGMPLAY_WARNINGS_AS_ERRORS "Traktuj ostrzezenia kodu vgmplay jako bledy (/WX)" OFF)

if(NOT WIN32)
    message(WARNING "vgmplay celuje wylacznie w Windows (SDL3 + D3D11); inne platformy nie sa wspierane.")
endif()

# /utf-8 globalnie (rowniez dla libvgm: zrodla maja znaki spoza ASCII w komentarzach).
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# Wspolne opcje kompilacji TYLKO dla kodu vgmplay (nie dla libvgm).
# Moduly linkuja vgmplay_options i dziedzicza C++20 oraz ostrzezenia.
# ---------------------------------------------------------------------------
add_library(vgmplay_options INTERFACE)
add_library(vgmplay::options ALIAS vgmplay_options)
target_compile_features(vgmplay_options INTERFACE cxx_std_20)
# Zamrozone naglowki kontraktow (include/vgmplay/*.hpp) widoczne dla wszystkich modulow i testow.
target_include_directories(vgmplay_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
    target_compile_options(vgmplay_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus)
    if(VGMPLAY_WARNINGS_AS_ERRORS)
        target_compile_options(vgmplay_options INTERFACE /WX)
    endif()
endif()
target_compile_definitions(vgmplay_options INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX)

# ---------------------------------------------------------------------------
# Wersja. Generuje <build>/generated/vgmplay/version.hpp (C++: kVersion, kGitCommit, ...)
# i version_rc.h (makra dla app.rc). Naglowek jest widoczny przez vgmplay::options:
#   #include <vgmplay/version.hpp>
# Skrot commita jest ustalany przy konfiguracji (i ponownie po zmianie HEAD, galezi, packed-refs albo indeksu).
# ---------------------------------------------------------------------------
set(VGMPLAY_VERSION_FULL "${PROJECT_VERSION}")
if(VGMPLAY_VERSION_SUFFIX)
    string(APPEND VGMPLAY_VERSION_FULL "-${VGMPLAY_VERSION_SUFFIX}")
endif()
set(VGMPLAY_GIT_COMMIT "unknown")
set(VGMPLAY_GIT_DIRTY 0)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _vgmplay_commit RESULT_VARIABLE _vgmplay_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_vgmplay_rc EQUAL 0 AND _vgmplay_commit)
        set(VGMPLAY_GIT_COMMIT "${_vgmplay_commit}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _vgmplay_status ERROR_QUIET)
        if(_vgmplay_status)
            set(VGMPLAY_GIT_DIRTY 1)
        endif()
        # Przekonfiguruj po kazdej zmianie, ktora zmienia commit albo "dirty": HEAD (przelaczenie galezi), plik rozwiazanej
        # galezi (refs/heads/<galaz>: nowy commit zmienia tylko ten plik), packed-refs (po gc/pack), index (git add/commit/
        # reset/checkout). Dziala tez w worktree, gdzie .git jest plikiem (--git-path daje sciezki wlasciwe worktree).
        # Ograniczenie: sama edycja sledzonego pliku bez `git add` nie zmienia zadnego z nich, wiec flaga dirty odswiezy sie
        # dopiero przy nastepnej rekonfiguracji (np. po zmianie CMakeLists.txt albo `git add`).
        set(_vgmplay_dep_names HEAD packed-refs index)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --symbolic-full-name HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _vgmplay_ref ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_vgmplay_ref AND NOT _vgmplay_ref STREQUAL "HEAD")   # odlaczony HEAD: samo "HEAD"
            list(APPEND _vgmplay_dep_names "${_vgmplay_ref}")
        endif()
        foreach(_vgmplay_dep IN LISTS _vgmplay_dep_names)
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${_vgmplay_dep}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE _vgmplay_dep_path ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(_vgmplay_dep_path AND EXISTS "${_vgmplay_dep_path}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vgmplay_dep_path}")
            endif()
        endforeach()
    endif()
endif()
# Numeryczne skladniki dla VERSIONINFO (cztery liczby; ostatnia = 0).
set(VGMPLAY_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(VGMPLAY_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(VGMPLAY_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(VGMPLAY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/version.hpp.in "${VGMPLAY_GENERATED_DIR}/vgmplay/version.hpp" @ONLY)
configure_file(cmake/version_rc.h.in "${VGMPLAY_GENERATED_DIR}/vgmplay/version_rc.h" @ONLY)
target_include_directories(vgmplay_options INTERFACE "${VGMPLAY_GENERATED_DIR}")
message(STATUS "vgmplay: wersja ${VGMPLAY_VERSION_FULL}, commit ${VGMPLAY_GIT_COMMIT}")

# vcpkg.json musi niesc te sama wersje (jedno zrodlo prawdy to project(VERSION)).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" _vgmplay_vcpkg_json)
    string(JSON _vgmplay_vcpkg_ver ERROR_VARIABLE _vgmplay_err GET "${_vgmplay_vcpkg_json}" "version-string")
    if(NOT _vgmplay_vcpkg_ver STREQUAL VGMPLAY_VERSION_FULL)
        message(WARNING "vcpkg.json: version-string '${_vgmplay_vcpkg_ver}' != '${VGMPLAY_VERSION_FULL}'")
    endif()
endif()

# ---------------------------------------------------------------------------
# Zaleznosci (vcpkg, tryb manifestu). Nazwy pakietow CMake to ZALOZENIA do
# weryfikacji po pierwszym configure. ZLIB jest wymagany (libvgm + loader).
# ---------------------------------------------------------------------------
find_package(ZLIB REQUIRED)
# Pozostale: brak = ostrzezenie, nie blad (zaslepki modulow jeszcze ich nie uzywaja).
find_package(SDL3 CONFIG QUIET)
find_package(imgui CONFIG QUIET)
# Port vcpkg "miniaudio" dostarcza tylko naglowek (brak konfiguracji CMake),
# wiec tworzymy wlasny cel INTERFACE vgmplay::miniaudio.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if(MINIAUDIO_INCLUDE_DIR)
    set(miniaudio_FOUND TRUE)
    add_library(vgmplay_miniaudio INTERFACE)
    add_library(vgmplay::miniaudio ALIAS vgmplay_miniaudio)
    target_include_directories(vgmplay_miniaudio INTERFACE "${MINIAUDIO_INCLUDE_DIR}")
endif()
find_package(nlohmann_json CONFIG QUIET)
foreach(_dep SDL3 imgui miniaudio nlohmann_json)
    if(${_dep}_FOUND)
        message(STATUS "vgmplay: znaleziono ${_dep}")
    else()
        message(WARNING "vgmplay: nie znaleziono pakietu ${_dep} (sprawdz vcpkg.json i nazwe pakietu CMake)")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# libvgm (external/libvgm, submodul). Nie modyfikujemy jego zrodel; sterujemy
# wylacznie opcjami CMake. Wartosci wymuszone (FORCE), bo sa decyzja projektu.
# Opis i uzasadnienie: docs/licensing/libvgm-audit.md.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libvgm/CMakeLists.txt")
    message(FATAL_ERROR
        "Brak external/libvgm. Uruchom: git submodule update --init --recursive")
endif()

macro(vgmplay_libvgm_opt name value)
    set(${name} ${value} CACHE BOOL "libvgm (ustawione przez vgmplay)" FORCE)
endmacro()

# -- Skladniki libvgm --
vgmplay_libvgm_opt(BUILD_LIBAUDIO OFF)   # wyjscie audio realizuje miniaudio (src/audio_out)
vgmplay_libvgm_opt(BUILD_LIBEMU   ON)
vgmplay_libvgm_opt(BUILD_LIBPLAYER ON)
vgmplay_libvgm_opt(BUILD_TESTS    OFF)   # audiotest/emutest/... wymagaja vgm-audio
vgmplay_libvgm_opt(BUILD_PLAYER   OFF)   # przykladowy player.cpp
vgmplay_libvgm_opt(BUILD_VGM2WAV  OFF)
vgmplay_libvgm_opt(USE_SANITIZERS OFF)   # jego modul Sanitizers jest nastawiony na gcc/clang
vgmplay_libvgm_opt(REALLY_NEED_CXX14 OFF) # wylacza rdzen ymfm (YM2414/OPZ); patrz audyt
set(LIBRARY_TYPE "STATIC" CACHE STRING "libvgm (ustawione przez vgmplay)" FORCE)

# -- utils: zlib z vcpkg, kodowanie znakow przez WinAPI (bez iconv) --
vgmplay_libvgm_opt(UTIL_LOADERS ON)
vgmplay_libvgm_opt(UTIL_THREADING ON)
vgmplay_libvgm_opt(UTIL_CHARSET_CONV ON)
vgmplay_libvgm_opt(UTIL_CHARCNV_ICONV OFF)
vgmplay_libvgm_opt(UTIL_CHARCNV_WINAPI ON)
# Pod MSVC libvgm domyslnie wskazuje WLASNE, dolaczone zlib.lib (32-bit, zlib 1.2.11)
# i libiconv.lib przez ZLIB_ROOT / Iconv_*. Zerujemy je w cache, zeby find_package(ZLIB)
# uzylo zlib z vcpkg (ich set(... CACHE ...) nie nadpisze juz istniejacych wartosci).
set(ZLIB_ROOT "" CACHE PATH "zerowane przez vgmplay (uzywamy zlib z vcpkg)" FORCE)
set(Iconv_INCLUDE_DIR "" CACHE PATH "zerowane przez vgmplay (bez iconv)" FORCE)
set(Iconv_LIBRARY "" CACHE FILEPATH "zerowane przez vgmplay (bez iconv)" FORCE)

# -- Rdzenie emulacji (emu) --
# Zamiast SNDEMU__ALL=ON wlaczamy urzadzenia pojedynczo, zeby pominac rdzenie
# problematyczne licencyjnie:
#   * Ootake_PSG.c (HuC6280)   - klauzula "uzycie komercyjne zabronione" => C6280: tylko MAME
#   * adlibemu_opl2/3 (OPL2/3) - pochodzi z kodu Kena Silvermana, licencja niejasna
#                                => YM3812/YMF262: MAME + Nuked
vgmplay_libvgm_opt(SNDEMU__ALL OFF)
foreach(_dev
        SN76496 YM2413 YM2612 YM2151 SEGAPCM RF5C68 YM2203 YM2608 YM2610
        YM3526 Y8950 YMF278B YMZ280B YMF271 AY8910 32X_PWM GAMEBOY NESAPU
        YMW258 UPD7759 MSM6258 MSM6295 K051649 K054539 C140 C219 K053260
        POKEY QSOUND SCSP WSWAN VBOY_VSU ES5503 ES5506 X1_010 C352 GA20
        MIKEY K007232 K005289 MSM5205 MSM5232 BSMT2000 ICS2115)
    vgmplay_libvgm_opt(SNDEMU_${_dev}_ALL ON)
endforeach()
# Urzadzenia z wybranymi rdzeniami:
vgmplay_libvgm_opt(SNDEMU_YM3812_MAME  ON)
vgmplay_libvgm_opt(SNDEMU_YM3812_NUKED ON)
vgmplay_libvgm_opt(SNDEMU_YMF262_MAME  ON)   # wymagany tez przez YMF278B
vgmplay_libvgm_opt(SNDEMU_YMF262_NUKED ON)
vgmplay_libvgm_opt(SNDEMU_C6280_MAME   ON)
vgmplay_libvgm_opt(SNDEMU_SAA1099_MAME ON)
vgmplay_libvgm_opt(SNDEMU_SAA1099_VB   ON)
# Jawnie wylaczone (nie ustawiamy *_ALL, wiec ponizsze zostaja OFF):
vgmplay_libvgm_opt(SNDEMU_C6280_OOTAKE OFF)
vgmplay_libvgm_opt(SNDEMU_YM3812_ADLIBEMU OFF)
vgmplay_libvgm_opt(SNDEMU_YMF262_ADLIBEMU OFF)

# Tryb ostrozny: dodatkowo wylacza rdzenie bez jawnej licencji w naglowku, o ile istnieje
# zamiennik z jawna licencja zgodna z GPL-2.0 (zob. audyt, sekcja "Rdzenie o niejasnej licencji").
# Konsekwencja: domyslnymi rdzeniami beda MAME/GPGX/Nuked zamiast NSFPlay, Maxim, Gens, superctr;
# brak emulacji FDS (jedyna implementacja to np_nes_fds.c z NSFPlay).
option(VGMPLAY_LIBVGM_STRICT_LICENSES
    "libvgm: pomin rdzenie bez jawnej licencji, majace zamiennik (NSFPlay, Maxim SN76489, Gens, QSound superctr)" OFF)
if(VGMPLAY_LIBVGM_STRICT_LICENSES)
    vgmplay_libvgm_opt(SNDEMU_SN76496_ALL OFF)
    vgmplay_libvgm_opt(SNDEMU_SN76496_MAME ON)
    vgmplay_libvgm_opt(SNDEMU_SN76496_MAXIM OFF)
    vgmplay_libvgm_opt(SNDEMU_YM2612_ALL OFF)
    vgmplay_libvgm_opt(SNDEMU_YM2612_GPGX ON)
    vgmplay_libvgm_opt(SNDEMU_YM2612_NUKED ON)
    vgmplay_libvgm_opt(SNDEMU_YM2612_GENS OFF)
    vgmplay_libvgm_opt(SNDEMU_RF5C68_ALL OFF)
    vgmplay_libvgm_opt(SNDEMU_RF5C68_MAME ON)
    vgmplay_libvgm_opt(SNDEMU_RF5C68_GENS OFF)
    vgmplay_libvgm_opt(SNDEMU_QSOUND_ALL OFF)
    vgmplay_libvgm_opt(SNDEMU_QSOUND_MAME ON)
    vgmplay_libvgm_opt(SNDEMU_QSOUND_CTR OFF)
    vgmplay_libvgm_opt(SNDEMU_NESAPU_ALL OFF)
    vgmplay_libvgm_opt(SNDEMU_NESAPU_MAME ON)
    vgmplay_libvgm_opt(SNDEMU_NESAPU_NSFPLAY OFF)
    vgmplay_libvgm_opt(SNDEMU_NESAPU_FDS OFF)
endif()

# EXCLUDE_FROM_ALL: instalacja libvgm (naglowki, .pc, config CMake) nie trafia do paczki ZIP;
# zbudowane zostana tylko biblioteki, od ktorych zalezy kod vgmplay.
add_subdirectory(external/libvgm EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Moduly vgmplay. Kazdy ma wlasny CMakeLists.txt i cel vgmplay_<modul>
# (alias vgmplay::<modul>). Zaleznosci miedzy modulami deklaruje sam modul.
# ---------------------------------------------------------------------------
set(VGMPLAY_MODULES
    core format loader engine audio_out viz chipstate playlist library
    settings input ui app)
foreach(_m IN LISTS VGMPLAY_MODULES)
    add_subdirectory(src/${_m})
endforeach()

# Narzedzie offline: render VGM do WAV tym samym dekoderem co silnik (tools/vgmrender).
add_subdirectory(tools/vgmrender)

if(VGMPLAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VGMPLAY_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()

# Pliki dolaczane do paczki ZIP (cmake --install <build> --prefix <katalog>).
# Plik wykonywalny doda install(TARGETS ...) w src/app/CMakeLists.txt.
# (Pelna paczka wydania: tools/package.ps1 - dodaje m.in. samples/, SOURCE.txt, SHA256SUMS.)
install(FILES LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES docs/user-guide.pl.md DESTINATION docs)
install(DIRECTORY licenses/ DESTINATION licenses)
install(FILES docs/licensing/libvgm-audit.md DESTINATION licenses)
