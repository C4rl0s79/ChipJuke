// Cel 3: decompressIfGzip - limit wyjscia musi byc zachowany.
#include <cstdint>
#include <cstdlib>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace vgmplay;
    const auto lim = fuzz::smallLimits();
    auto d = format::decompressIfGzip({data, size}, lim);
    if (d && d.value().bytes.size() > lim.maxDecompressedBytes) std::abort();
    return 0;
}
