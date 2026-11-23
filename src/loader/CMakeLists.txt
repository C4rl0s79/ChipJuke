# Modul: loader
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_loader STATIC
    loader_stub.cpp
)
add_library(vgmplay::loader ALIAS vgmplay_loader)
target_include_directories(vgmplay_loader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_loader PUBLIC vgmplay::options)
# Dekompresja VGZ/VGMZ (gzip). ZLIB::ZLIB pochodzi z find_package(ZLIB) w korzeniu.
target_link_libraries(vgmplay_loader PUBLIC ZLIB::ZLIB)
