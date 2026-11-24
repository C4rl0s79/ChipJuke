// Cel libFuzzer: MetaCache::fromJson (cialo w fuzz_targets.hpp, wspolne z testem regresyjnym tests/fuzz_regress_test.cpp).
#include <cstdint>
#include <cstdlib>

#include "fuzz_targets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (!vgmplay::fuzz::runLibraryCache({data, size})) std::abort();
    return 0;
}
