// Zaslepka modulu "audio_out". Usunac, gdy modul dostanie wlasne zrodla
// (i zaktualizowac liste zrodel w CMakeLists.txt tego katalogu).
namespace vgmplay::audio_out {

int module_stub() noexcept
{
    return 0;
}

} // namespace vgmplay::audio_out
