# Modul: ui
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_ui STATIC
    channel_mix.cpp
    channel_scopes.cpp
    channel_widgets.cpp
    format.cpp
    nav_input.cpp
    pattern_view.cpp
    player_screen.cpp
    scope_buffer.cpp
    strings.cpp
    theme.cpp
    track_meta.cpp
    playlist_panel.cpp
    dialogs.cpp
    bindings_screen.cpp
    file_browser.cpp
    file_browser_screen.cpp
)
add_library(vgmplay::ui ALIAS vgmplay_ui)
target_include_directories(vgmplay_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_ui PUBLIC vgmplay::options vgmplay::playlist vgmplay::input imgui::imgui SDL3::SDL3)
