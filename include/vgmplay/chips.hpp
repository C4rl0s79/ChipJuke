// vgmplay - kontrakt: chips.hpp
// Wspolne slownictwo ukladow dzwiekowych (niezalezne od libvgm). Uzywane przez format,
// layout/decoder/engine, settings i chipstate. Wartosci liczbowe NIE sa trwale
// (nie zapisuj ich do plikow) - do JSON uzywaj chipTypeName().
#pragma once

#include <cstdint>
#include <string_view>

namespace vgmplay {

enum class ChipType : std::uint8_t {
    Unknown = 0,
    SN76496, YM2413, YM2612, YM2151, SegaPcm, RF5C68, YM2203, YM2608, YM2610,
    YM3812, YM3526, Y8950, YMF262, YMF278B, YMF271, YMZ280B, RF5C164, PWM32X,
    AY8910, GameBoyDMG, NesApu, MultiPCM, UPD7759, OKIM6258, OKIM6295, K051649,
    K054539, HuC6280, C140, K053260, Pokey, QSound, SCSP, WonderSwan, VirtualBoyVSU,
    SAA1099, ES5503, ES5506, X1010, C352, GA20, Mikey, K007232, K005289, MSM5205,
    MSM5232, BSMT2000, ICS2115, C219, YMW258,
    Count_ // nie uzywac jako wartosci; ostatni element
};

// Stabilny identyfikator tekstowy ("YM2612", "SN76496", ...); "Unknown" dla wartosci spoza
// zakresu. Wskaznik na dane statyczne. Implementacja: modul format.
[[nodiscard]] std::string_view chipTypeName(ChipType type) noexcept;

// Wybor rdzenia emulacji dla ukladu. coreId = kod rdzenia (FCC libvgm) jako token
// nieprzezroczysty; 0 = domyslny rdzen vgmplay (tabela domyslnych rdzeni w engine).
// Poza modulem engine traktuj coreId jako wartosc odczytana z CoreInfo.
struct CoreChoice {
    ChipType type = ChipType::Unknown;
    std::uint8_t instance = 0;   // 0 lub 1 (drugi uklad tego samego typu)
    std::uint32_t coreId = 0;
    bool operator==(const CoreChoice&) const = default;
};

} // namespace vgmplay
