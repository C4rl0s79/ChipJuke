# Modul: format
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_format STATIC
    format_stub.cpp
)
add_library(vgmplay::format ALIAS vgmplay_format)
target_include_directories(vgmplay_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_format PUBLIC vgmplay::options)
