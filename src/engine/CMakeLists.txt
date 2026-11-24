# Modul: engine (dekoder VGM na libvgm + petla watku silnika)
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_engine STATIC
    channel_scopes.cpp
    chip_map.cpp
    engine.cpp
    vgm_decoder.cpp
)
add_library(vgmplay::engine ALIAS vgmplay_engine)
target_include_directories(vgmplay_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_engine PUBLIC vgmplay::options)
# Dekoder waliduje wejscie sam (obrona w glebi): format::parseVgmHeader/validateCommandStream.
target_link_libraries(vgmplay_engine PUBLIC vgmplay::format)
# PcmRing (naglowek) i konsument-callback zyja w audio_out; silnik jest producentem.
target_link_libraries(vgmplay_engine PUBLIC vgmplay::audio_out)
# Silnik odtwarzania opiera sie na libvgm (cele z external/libvgm).
# vgm-player ciagnie vgm-emu i vgm-utils (zlib, watki, konwersja stron kodowych).
# Naglowki libvgm sa dolaczane wylacznie przez src/engine/libvgm_headers.hpp (bez ostrzezen /W4).
target_link_libraries(vgmplay_engine PUBLIC vgm-player vgm-emu vgm-utils)
# stdtype.h libvgm wybiera typy wg HAVE_STDINT_H (libvgm ustawia go tylko dla swoich celow).
target_compile_definitions(vgmplay_engine PRIVATE HAVE_STDINT_H VGM_LITTLE_ENDIAN)
