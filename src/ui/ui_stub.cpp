// Zaslepka modulu "ui": smoke_test.cpp (tests/) wola module_stub() kazdego modulu.
// Usunac razem z wywolaniem w testach, gdy modul dostanie wlasne testy.
namespace vgmplay::ui {

int module_stub() noexcept
{
    return 0;
}

} // namespace vgmplay::ui
