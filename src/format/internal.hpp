// vgmplay - modul format: deklaracje wewnetrzne (NIE czesc kontraktu).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"

namespace vgmplay::format::detail {

inline std::uint16_t rd16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t rd32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Czyta bajty z bufora, ktorego "wirtualny" rozmiar jest wiekszy niz dane (reszta = zera);
// odwzorowuje _hdrBuffer libvgm (memset 0 + memcpy dataOfs bajtow).
struct PaddedHeader {
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;     // ile bajtow naprawde istnieje

    [[nodiscard]] std::uint8_t u8(std::size_t ofs) const noexcept
    {
        return ofs < length ? data[ofs] : std::uint8_t{0};
    }
    [[nodiscard]] std::uint16_t u16(std::size_t ofs) const noexcept
    {
        return static_cast<std::uint16_t>(u8(ofs) | (u8(ofs + 1) << 8));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t ofs) const noexcept
    {
        return static_cast<std::uint32_t>(u16(ofs)) | (static_cast<std::uint32_t>(u16(ofs + 2)) << 16);
    }
};

// Liczba pol zegara w naglowku VGM = rozmiar VGMPlayer::_DEV_LIST (libvgm).
inline constexpr std::size_t kVgmChipCount = 48;

// Offset pola zegara ukladu o danym indeksie (VGMPlayer::_CHIPCLK_OFS).
inline constexpr std::uint16_t kChipClockOfs[kVgmChipCount] = {
    0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x44, 0x48,
    0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68,
    0x6C, 0x70, 0x74, 0x80, 0x84, 0x88, 0x8C, 0x90,
    0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4,
    0xB8, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xDC,
    0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC,
};

// Indeks ukladu (0..47) -> ChipType. Zgodnosc: wartosc enuma = indeks + 1 (static_assert w chips.cpp).
[[nodiscard]] inline ChipType chipTypeFromIndex(std::size_t idx) noexcept
{
    return static_cast<ChipType>(idx + 1);
}

// ChipType -> indeks ukladu w naglowku; kVgmChipCount gdy brak (Unknown/Count_).
// C219 wspoldzieli pole zegara z C140 (indeks 0x1C).
[[nodiscard]] inline std::size_t chipIndexFromType(ChipType t) noexcept
{
    if (t == ChipType::C219) return 0x1C;
    const auto v = static_cast<std::size_t>(t);
    return (v >= 1 && v <= kVgmChipCount) ? v - 1 : kVgmChipCount;
}

inline constexpr std::uint64_t kMinVgmHeader = 0x38;

// UTF-16LE (wskaznik na bajty, liczba bajtow) -> UTF-8, sanityzacja. Implementacja: gd3.cpp.
struct Utf8Result {
    std::string text;
    bool sanitized = false;
    bool truncated = false;
};
[[nodiscard]] Utf8Result utf16leToUtf8(std::span<const std::uint8_t> bytes, std::uint32_t maxBytes);

} // namespace vgmplay::format::detail
