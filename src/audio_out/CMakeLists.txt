# Modul: audio_out
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_audio_out STATIC
    audio_out_stub.cpp
)
add_library(vgmplay::audio_out ALIAS vgmplay_audio_out)
target_include_directories(vgmplay_audio_out PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_audio_out PUBLIC vgmplay::options)
