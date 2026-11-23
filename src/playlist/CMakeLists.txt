# Modul: playlist
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_playlist STATIC
    playlist_stub.cpp
)
add_library(vgmplay::playlist ALIAS vgmplay_playlist)
target_include_directories(vgmplay_playlist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_playlist PUBLIC vgmplay::options)
