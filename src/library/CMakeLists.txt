# Modul: library
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_library STATIC
    library_stub.cpp
)
add_library(vgmplay::library ALIAS vgmplay_library)
target_include_directories(vgmplay_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_library PUBLIC vgmplay::options)
