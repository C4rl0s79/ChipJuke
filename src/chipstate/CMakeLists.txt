# Modul: chipstate
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_chipstate STATIC
    chipstate_stub.cpp
)
add_library(vgmplay::chipstate ALIAS vgmplay_chipstate)
target_include_directories(vgmplay_chipstate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_chipstate PUBLIC vgmplay::options)
