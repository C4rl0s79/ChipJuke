# Modul: format (czyste funkcje: gzip, naglowek VGM, GD3, pre-skan strumienia). Bez libvgm.
add_library(vgmplay_format STATIC
    chips.cpp
    gd3.cpp
    gzip.cpp
    header.cpp
    stream.cpp
)
add_library(vgmplay::format ALIAS vgmplay_format)
target_include_directories(vgmplay_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_format PUBLIC vgmplay::options)
# Dekompresja gzip (wlasny inflate z limitami). ZLIB::ZLIB pochodzi z find_package(ZLIB) w korzeniu.
target_link_libraries(vgmplay_format PRIVATE ZLIB::ZLIB)
