# Modul: app - AppController (biblioteka, linkowana tez przez testy) + plik wykonywalny vgmplay.
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_app STATIC
    app_controller.cpp
)
add_library(vgmplay::app ALIAS vgmplay_app)
target_include_directories(vgmplay_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vgmplay_app PUBLIC
    vgmplay::options
    vgmplay::ui
    vgmplay::loader
    vgmplay::engine
    vgmplay::audio_out)

add_executable(vgmplay
    main.cpp
    file_dialog.cpp
    app.rc
)
target_include_directories(vgmplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vgmplay PRIVATE
    vgmplay::app
    imgui::imgui
    SDL3::SDL3
    d3d11 dxgi d3dcompiler shell32)
set_property(SOURCE app.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/fonts/BIZUDGothic-Regular.ttf")

if(MSVC)
    # Release: podsystem WINDOWS (bez konsoli); Debug: CONSOLE (logi na stderr).
    # Wlasne main() zamiast WinMain => jawny punkt wejscia.
    target_link_options(vgmplay PRIVATE
        "$<IF:$<CONFIG:Debug>,/SUBSYSTEM:CONSOLE,/SUBSYSTEM:WINDOWS>"
        /ENTRY:mainCRTStartup)
endif()

install(TARGETS vgmplay RUNTIME DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt"
        DESTINATION licenses RENAME BIZUDGothic-OFL.txt)
