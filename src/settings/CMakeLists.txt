# Modul: settings (JSON, migracje, lokalizacja pliku, zapis atomowy; pliki - tylko z workera).
# fs_util.hpp to wewnetrzny naglowek wspoldzielony z modulami playlist i library.
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_settings STATIC
    settings.cpp
)
add_library(vgmplay::settings ALIAS vgmplay_settings)
target_include_directories(vgmplay_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(vgmplay_settings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vgmplay_settings
    PUBLIC vgmplay::options vgmplay::format
    PRIVATE nlohmann_json::nlohmann_json)
