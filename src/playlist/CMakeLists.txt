# Modul: playlist (model, JSON, M3U/M3U8; pliki - tylko z workera).
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_playlist STATIC
    playlist_model.cpp
    playlist_io.cpp
    m3u.cpp
)
add_library(vgmplay::playlist ALIAS vgmplay_playlist)
target_include_directories(vgmplay_playlist PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# fs_util.hpp (zapis atomowy, UTF-8) jest wspolnym, wewnetrznym naglowkiem modulu settings.
target_include_directories(vgmplay_playlist PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../settings)
target_link_libraries(vgmplay_playlist
    PUBLIC vgmplay::options
    PRIVATE nlohmann_json::nlohmann_json)
