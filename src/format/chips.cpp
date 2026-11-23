// vgmplay - modul format: nazwy ukladow (chips.hpp).
#include "vgmplay/chips.hpp"

#include <array>
#include <cstddef>

#include "internal.hpp"

namespace vgmplay {
namespace {

// Kolejnosc IDENTYCZNA jak w enum ChipType (od SN76496). Nazwy = identyfikatory enumeratorow;
// trafiaja do JSON-a ustawien, wiec sa STABILNE - nie zmieniac.
constexpr std::array<std::string_view, static_cast<std::size_t>(ChipType::Count_)> kNames = {
    "Unknown",
    "SN76496", "YM2413", "YM2612", "YM2151", "SegaPcm", "RF5C68", "YM2203", "YM2608", "YM2610",
    "YM3812", "YM3526", "Y8950", "YMF262", "YMF278B", "YMF271", "YMZ280B", "RF5C164", "PWM32X",
    "AY8910", "GameBoyDMG", "NesApu", "MultiPCM", "UPD7759", "OKIM6258", "OKIM6295", "K051649",
    "K054539", "HuC6280", "C140", "K053260", "Pokey", "QSound", "SCSP", "WonderSwan", "VirtualBoyVSU",
    "SAA1099", "ES5503", "ES5506", "X1010", "C352", "GA20", "Mikey", "K007232", "K005289", "MSM5205",
    "MSM5232", "BSMT2000", "ICS2115", "C219",
};

// 48 ukladow naglowka VGM (libvgm _DEV_LIST) + C219 + Unknown.
static_assert(static_cast<std::size_t>(ChipType::Count_) == format::detail::kVgmChipCount + 2);
static_assert(static_cast<std::size_t>(ChipType::C219) == format::detail::kVgmChipCount + 1);
// Spot-checki zgodnosci wartosc = indeks_w_DEV_LIST + 1 (por. DEVID_* w emu/SoundDevs.h).
static_assert(static_cast<std::size_t>(ChipType::YM2612) == 0x02 + 1);
static_assert(static_cast<std::size_t>(ChipType::RF5C164) == 0x10 + 1);
static_assert(static_cast<std::size_t>(ChipType::MultiPCM) == 0x15 + 1);
static_assert(static_cast<std::size_t>(ChipType::HuC6280) == 0x1B + 1);
static_assert(static_cast<std::size_t>(ChipType::ICS2115) == 0x2F + 1);

} // namespace

std::string_view chipTypeName(ChipType type) noexcept
{
    const auto v = static_cast<std::size_t>(type);
    if (v >= kNames.size()) return "Unknown";
    return kNames[v];
}

} // namespace vgmplay
