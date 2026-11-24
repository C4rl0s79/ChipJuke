// Zaslepka modulu chipstate: buildPattern zwraca wzorzec bez zdekodowanych kanalow.
// Do zastapienia implementacja (E1.5, backend-implementer).
#include "vgmplay/chipstate.hpp"

namespace vgmplay {
namespace chipstate {
int module_stub() noexcept { return 0; }

Result<Pattern> buildPattern(std::span<const std::uint8_t>, const format::HeaderInfo&,
                             const format::StreamReport& stream, const PatternOptions& options)
{
    Pattern p;
    p.rowTicks = options.rowTicks == 0 ? 735u : options.rowTicks;
    p.rowCount = stream.totalSamples / p.rowTicks + 1;
    return p;
}
} // namespace chipstate
} // namespace vgmplay
