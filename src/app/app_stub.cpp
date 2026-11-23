// Zaslepka modulu "app": smoke_test.cpp (tests/) wola module_stub() kazdego modulu.
// Usunac razem z wywolaniem w testach, gdy modul dostanie wlasne testy.
namespace vgmplay::app {

int module_stub() noexcept
{
    return 0;
}

} // namespace vgmplay::app
