# Modul: loader (plik/pamiec -> LoadedTrack; blokujacy, tylko z workera).
add_library(vgmplay_loader STATIC
    loader.cpp
)
add_library(vgmplay::loader ALIAS vgmplay_loader)
target_include_directories(vgmplay_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_loader PUBLIC vgmplay::options vgmplay::format)
