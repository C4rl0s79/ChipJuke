// Test wiazania modulow: sprawdza, ze wszystkie biblioteki-zaslepki sie linkuja.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

namespace vgmplay {
namespace core { int module_stub() noexcept; }
namespace viz { int module_stub() noexcept; }
namespace chipstate { int module_stub() noexcept; }
namespace playlist { int module_stub() noexcept; }
namespace library { int module_stub() noexcept; }
namespace settings { int module_stub() noexcept; }
namespace input { int module_stub() noexcept; }
namespace ui { int module_stub() noexcept; }
namespace app { int module_stub() noexcept; }
} // namespace vgmplay

TEST_CASE("zaslepki modulow linkuja sie poprawnie")
{
    using namespace vgmplay;
    CHECK(core::module_stub() == 0);
    CHECK(viz::module_stub() == 0);
    CHECK(chipstate::module_stub() == 0);
    CHECK(playlist::module_stub() == 0);
    CHECK(library::module_stub() == 0);
    CHECK(settings::module_stub() == 0);
    CHECK(input::module_stub() == 0);
    CHECK(ui::module_stub() == 0);
    CHECK(app::module_stub() == 0);
}
