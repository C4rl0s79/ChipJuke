# Modul: audio_out (PcmRing + wyjscie miniaudio/WASAPI)
# Dopisuj zrodla i zaleznosci tylko w tym pliku.
add_library(vgmplay_audio_out STATIC
    pcm_ring.cpp
    audio_out.cpp
)
add_library(vgmplay::audio_out ALIAS vgmplay_audio_out)
target_include_directories(vgmplay_audio_out PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vgmplay_audio_out PUBLIC vgmplay::options)
# miniaudio tylko wewnetrznie (naglowek nie wycieka do konsumentow modulu).
target_link_libraries(vgmplay_audio_out PRIVATE vgmplay::miniaudio)
