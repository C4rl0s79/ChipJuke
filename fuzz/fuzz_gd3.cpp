// Cel 2: parseGd3. Wejscie = chunk GD3 (od "Gd3 "), doklejony za 0x40 B zerowego naglowka; gd3Offset = 0x40.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "fuzz_common.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace vgmplay;
    const auto lim = fuzz::smallLimits();
    std::vector<std::uint8_t> file(0x40, 0);
    file.insert(file.end(), data, data + size);
    format::HeaderInfo h;
    h.gd3Offset = 0x40;
    h.fileSize = file.size();
    h.dataEnd = h.fileSize;
    auto g = format::parseGd3(file, h, lim);
    if (g) {
        const auto& t = g.value();
        for (const std::string* f : {&t.trackEn, &t.trackJp, &t.gameEn, &t.gameJp, &t.systemEn, &t.systemJp,
                                     &t.authorEn, &t.authorJp, &t.releaseDate, &t.converter, &t.notes})
            if (f->size() > lim.maxTagFieldBytes) std::abort();
    }
    return 0;
}
