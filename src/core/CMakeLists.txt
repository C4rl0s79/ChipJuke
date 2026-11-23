# Modul: core
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_core STATIC
    core_stub.cpp
)
add_library(vgmplay::core ALIAS vgmplay_core)
target_include_directories(vgmplay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_core PUBLIC vgmplay::options)
