// vgmplay - modul chipstate: kolejnosc kanalow ukladow (wewnetrzne).
#pragma once

#include <cstdint>
#include <vector>

#include "vgmplay/chipstate.hpp"

namespace vgmplay::chipstate::detail {

// Kanaly jednej instancji ukladu w kolejnosci ChannelLayout silnika (glowne, potem polaczone); channelIdx
// liczony od 0 w obrebie ukladu. `flags` = ChipEntry::flags (NES: bit 31 = FDS). Pusty wynik = uklad bez
// kanalow w silniku (ES5506) albo nieznany.
[[nodiscard]] std::vector<PatternChannel> channelsFor(ChipType type, std::uint8_t instance, std::uint32_t flags);

} // namespace vgmplay::chipstate::detail
