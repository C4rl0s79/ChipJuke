# Modul: settings
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_settings STATIC
    settings_stub.cpp
)
add_library(vgmplay::settings ALIAS vgmplay_settings)
target_include_directories(vgmplay_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_settings PUBLIC vgmplay::options)
