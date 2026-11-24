# Modul: chipstate (wzorzec w stylu trackera z zapisow rejestrow; bez libvgm).
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_chipstate STATIC
    chip_layout.cpp
    dec_fm.cpp
    dec_oki.cpp
    dec_opl.cpp
    dec_psg.cpp
    pattern.cpp
)
add_library(vgmplay::chipstate ALIAS vgmplay_chipstate)
target_include_directories(vgmplay_chipstate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Tabela dlugosci komend (src/format/cmdtable.hpp) - wspolna z pre-skanem; tylko naglowek, prywatnie.
target_include_directories(vgmplay_chipstate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../format)
target_link_libraries(vgmplay_chipstate PUBLIC vgmplay::options vgmplay::format)
