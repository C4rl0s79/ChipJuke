# Cele fuzzingu (libFuzzer + ASan, clang-cl). Wlaczane opcja VGMPLAY_BUILD_FUZZ (preset clang-cl-asan-fuzz).
# Budowa (clang-cl musi byc w PATH, np. z VS BuildTools: VC\Tools\Llvm\x64\bin):
#   cmake --preset clang-cl-asan-fuzz && cmake --build --preset clang-cl-asan-fuzz
# Uruchomienie (seed = korpus testowy; pierwszy katalog jest roboczy):
#   fuzz_vgm_header.exe -max_total_time=60 -dict=fuzz/vgm.dict <katalog_roboczy> tests/data/valid tests/data/malformed
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

function(vgmplay_add_fuzz name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_libraries(${name} PRIVATE vgmplay::options ${ARGN} "${_vgmplay_fuzz_lib}")
endfunction()

vgmplay_add_fuzz(fuzz_vgm_header vgmplay::format)
vgmplay_add_fuzz(fuzz_gd3        vgmplay::format)
vgmplay_add_fuzz(fuzz_gzip       vgmplay::format)
vgmplay_add_fuzz(fuzz_stream     vgmplay::format)
vgmplay_add_fuzz(fuzz_decode     vgmplay::loader vgmplay::engine)
