# Modul: ui
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_ui STATIC
    mockup.cpp
    nav_input.cpp
    strings.cpp
    theme.cpp
    ui_stub.cpp
)
add_library(vgmplay::ui ALIAS vgmplay_ui)
target_include_directories(vgmplay_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_ui PUBLIC vgmplay::options imgui::imgui SDL3::SDL3)
