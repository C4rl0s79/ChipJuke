# Modul: input
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_input STATIC
    router.cpp
    bindings.cpp
    bindings_edit.cpp
)
add_library(vgmplay::input ALIAS vgmplay_input)
target_include_directories(vgmplay_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_input PUBLIC vgmplay::options)
