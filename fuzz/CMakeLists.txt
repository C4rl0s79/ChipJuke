# Cele fuzzingu (libFuzzer + ASan, clang-cl). Wlaczane opcja VGMPLAY_BUILD_FUZZ (preset clang-cl-asan-fuzz).
# Budowa (clang-cl musi byc w PATH, np. z VS BuildTools: VC\Tools\Llvm\x64\bin):
#   cmake --preset clang-cl-asan-fuzz && cmake --build --preset clang-cl-asan-fuzz
# Uruchomienie (seed = korpus testowy; pierwszy katalog jest roboczy):
#   fuzz_vgm_header.exe -max_total_time=60 -dict=fuzz/vgm.dict <katalog_roboczy> tests/data/valid tests/data/malformed
# Cele plikowe: fuzz_settings (settings.dict), fuzz_playlist_json (playlist.dict), fuzz_m3u (m3u.dict),
# fuzz_library_cache (library_cache.dict), fuzz_pattern (vgm.dict). Korpusy regresyjne: fuzz/corpus/<cel>/
# (odtwarzane przez tests/fuzz_regress_test.cpp na buildzie MSVC). Patrz docs/testing/fuzzing.md.
#
# Runtime libFuzzer (clang_rt.fuzzer) dolaczamy jawnie; katalog biblioteki pobieramy od kompilatora
# (-print-resource-dir). Biblioteka jest zbudowana z /MT, wiec preset uzywa statycznego CRT (x64-windows-static)
# + ASan w wariancie "dll runtime + static thunk" (clang_rt.asan_dynamic + clang_rt.asan_static_runtime_thunk).
# Wymaga env VGMPLAY_CLANG_RT_DIR (katalog lib/windows kompilatora) i clang_rt.asan_dynamic-x86_64.dll w PATH.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                OUTPUT_VARIABLE _vgmplay_clang_res OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
file(TO_CMAKE_PATH "${_vgmplay_clang_res}" _vgmplay_clang_res)
set(_vgmplay_fuzz_libdir "${_vgmplay_clang_res}/lib/windows")
if(NOT EXISTS "${_vgmplay_fuzz_libdir}/clang_rt.fuzzer-x86_64.lib")
    message(FATAL_ERROR "Brak clang_rt.fuzzer-x86_64.lib w ${_vgmplay_fuzz_libdir} (clang-cl z komponentem LLVM?)")
endif()

set(_vgmplay_fuzz_lib "${_vgmplay_fuzz_libdir}/clang_rt.fuzzer-x86_64.lib")

# Pokrycie dla libFuzzera: moduly vgmplay i libvgm musza byc skompilowane z instrumentacja
# (-fsanitize=fuzzer-no-link), inaczej libFuzzer widzi tylko kod samego celu (cov ~30 pkt) i nie ma
# sprzezenia zwrotnego. Opcje dodajemy tu, zeby nie ruszac CMakeLists modulow.
foreach(_t IN ITEMS vgmplay_format vgmplay_loader vgmplay_engine vgmplay_chipstate vgmplay_playlist
                    vgmplay_library vgmplay_settings vgmplay_input vgm-player vgm-emu vgm-utils)
    if(TARGET ${_t})
        target_compile_options(${_t} PRIVATE -fsanitize=fuzzer-no-link)
    endif()
endforeach()

option(VGMPLAY_FUZZ_SKIP_OPEN_BUGS "Fuzzer pomija wejscia trafiajace w znane, nienaprawione bledy src/ (fuzz/crashes)" OFF)

set(VGMPLAY_FUZZ_MODULES vgmplay::format vgmplay::loader vgmplay::engine vgmplay::chipstate
    vgmplay::playlist vgmplay::library vgmplay::settings)

function(vgmplay_add_fuzz name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link)
    if(VGMPLAY_FUZZ_SKIP_OPEN_BUGS)
        target_compile_definitions(${name} PRIVATE VGMPLAY_FUZZ_SKIP_OPEN_BUGS=1)
    endif()
    # Wszystkie cele zawieraja ciala z fuzz_targets.hpp (jeden naglowek), wiec linkuja komplet modulow.
    target_link_libraries(${name} PRIVATE vgmplay::options ${ARGN} "${_vgmplay_fuzz_lib}")
endfunction()

vgmplay_add_fuzz(fuzz_vgm_header ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_gd3 ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_gzip ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_stream ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_decode ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_settings ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_playlist_json ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_m3u ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_library_cache ${VGMPLAY_FUZZ_MODULES})
vgmplay_add_fuzz(fuzz_pattern ${VGMPLAY_FUZZ_MODULES})
