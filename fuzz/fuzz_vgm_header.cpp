// Cel 1: bajty -> decompressIfGzip -> parseVgmHeader -> parseGd3 -> validateCommandStream.
// Niezmienniki: brak wyjatku/UB (ASan), offsety <= fileSize, determinizm.
#include <cstdint>
#include <cstdlib>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace vgmplay;
    const auto lim = fuzz::smallLimits();
    auto d = format::decompressIfGzip({data, size}, lim);
    if (!d) return 0;
    const auto& raw = d.value().bytes;
    auto h = format::parseVgmHeader(raw, lim);
    if (!h) return 0;
    const auto& hi = h.value();
    if (hi.dataOffset > hi.dataEnd || hi.dataEnd > hi.fileSize || hi.gd3Offset > hi.fileSize ||
        hi.loopOffset > hi.fileSize)
        std::abort();
    (void)format::parseGd3(raw, hi, lim);
    auto s1 = format::validateCommandStream(raw, hi, lim);
    auto s2 = format::validateCommandStream(raw, hi, lim);
    if (s1.ok() != s2.ok()) std::abort();
    return 0;
}
