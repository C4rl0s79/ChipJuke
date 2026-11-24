// Test wiazania modulow: sprawdza, ze wszystkie biblioteki-zaslepki sie linkuja.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

namespace vgmplay {
namespace core { int module_stub() noexcept; }
namespace viz { int module_stub() noexcept; }
namespace input { int module_stub() noexcept; }
} // namespace vgmplay

TEST_CASE("zaslepki modulow linkuja sie poprawnie")
{
    using namespace vgmplay;
    CHECK(core::module_stub() == 0);
    CHECK(viz::module_stub() == 0);
    CHECK(input::module_stub() == 0);
}
