# vgmrender: render offline VGM -> WAV tym samym dekoderem (IDecoder) co silnik.
# Do testow golden i recznego odsluchu. Bez zaleznosci od audio_out/miniaudio w czasie dzialania.
add_executable(vgmrender main.cpp)
target_link_libraries(vgmrender PRIVATE vgmplay::options vgmplay::engine vgmplay::loader)
