# Modul: viz
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_viz STATIC
    viz_stub.cpp
)
add_library(vgmplay::viz ALIAS vgmplay_viz)
target_include_directories(vgmplay_viz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_viz PUBLIC vgmplay::options)
