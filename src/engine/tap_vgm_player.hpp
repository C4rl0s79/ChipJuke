// vgmplay - engine (wewnetrzne): TapVgmPlayer = VGMPlayer + dostep do skladowych `protected`.
// ADR 0002 pkt 1/5/6: podklasa jest miejscem na pozniejsze "tapy" (bufory per uklad - wariant (b),
// hook zapisow rejestrow - wariant B) bez modyfikowania external/libvgm. W E1 daje tylko to, czego
// publiczne API VGMPlayer nie udostepnia: liczbe i nazwy FAKTYCZNIE uruchomionych urzadzen
// (uklady, ktorych rdzen nie wystartowal, libvgm po cichu pomija).
#pragma once

#include <cstddef>
#include <string>

#include "libvgm_headers.hpp"

namespace vgmplay::engine::detail {

class TapVgmPlayer : public VGMPlayer {
public:
    // Liczba urzadzen glownych utworzonych przez Start() (bez ukladow polaczonych).
    [[nodiscard]] std::size_t startedDeviceCount() const noexcept { return _devices.size(); }
    // Typ ukladu wg VGM (indeks z naglowka, np. 0x05 RF5C68, 0x10 RF5C164) urzadzenia o indeksie i.
    [[nodiscard]] unsigned vgmChipTypeOf(std::size_t i) const noexcept
    {
        return i < _devices.size() ? _devices[i].vgmChipType : 0xFFu;
    }
    // E1.5: czy rdzen urzadzenia (chipType, chipID) i jego ukladu polaczonego ma SetMuteMask.
    // Bit 0 = uklad glowny, bit 1 = polaczony. 0 = nie znaleziono / brak.
    [[nodiscard]] unsigned muteSupport(unsigned chipType, unsigned chipId) const noexcept
    {
        for (const CHIP_DEVICE& d : _devices) {
            if (d.chipType != chipType || d.chipID != chipId) continue;
            unsigned m = 0, bit = 1;
            for (const VGM_BASEDEV* b = &d.base; b != nullptr && bit <= 2; b = b->linkDev, bit <<= 1)
                if (b->defInf.dataPtr != nullptr && b->defInf.devDef != nullptr &&
                    b->defInf.devDef->SetMuteMask != nullptr)
                    m |= bit;
            return m;
        }
        return 0;
    }
    [[nodiscard]] const char* deviceName(std::size_t i) const noexcept
    {
        return i < _devNames.size() ? _devNames[i].c_str() : "";
    }
};

} // namespace vgmplay::engine::detail
