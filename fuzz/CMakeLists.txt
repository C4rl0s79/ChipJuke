# Cele fuzzingu (libFuzzer, clang-cl). Wlaczane opcja VGMPLAY_BUILD_FUZZ (preset clang-cl-asan-fuzz).
# Przyklad celu (dodac, gdy powstanie parser w src/format):
#   add_executable(fuzz_vgm_header fuzz_vgm_header.cpp)
#   target_compile_options(fuzz_vgm_header PRIVATE -fsanitize=fuzzer)
#   target_link_options(fuzz_vgm_header PRIVATE -fsanitize=fuzzer)
#   target_link_libraries(fuzz_vgm_header PRIVATE vgmplay::format)
message(STATUS "vgmplay: VGMPLAY_BUILD_FUZZ=ON, brak jeszcze celow fuzzingu")
