// Cel 4: validateCommandStream na surowych bajtach (bez gzip), z naglowkiem z parseVgmHeader.
#include <cstdint>
#include <cstdlib>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace vgmplay;
    const auto lim = fuzz::smallLimits();
    auto h = format::parseVgmHeader({data, size}, lim);
    if (!h) return 0;
    auto s = format::validateCommandStream({data, size}, h.value(), lim);
    if (s && s.value().totalSamples > lim.maxTotalSamples) std::abort();
    return 0;
}
