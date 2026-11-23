# Modul: engine
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_engine STATIC
    engine_stub.cpp
)
add_library(vgmplay::engine ALIAS vgmplay_engine)
target_include_directories(vgmplay_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_engine PUBLIC vgmplay::options)
# Silnik odtwarzania opiera sie na libvgm (cele z external/libvgm).
# vgm-player ciagnie vgm-emu i vgm-utils (zlib, watki, konwersja stron kodowych).
target_link_libraries(vgmplay_engine PUBLIC vgm-player vgm-emu vgm-utils)
