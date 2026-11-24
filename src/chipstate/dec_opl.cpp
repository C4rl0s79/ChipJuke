// vgmplay - modul chipstate: rodzina OPL - YM2413 (OPLL), YM3812/YM3526 (OPL2/OPL), YMF262 (OPL3).
//
// Kanaly (kolejnosc jak w silniku): melodyczne 1..9 (OPL3: 1..18; kanaly 10..18 = port 1), potem perkusja
// "Bass Drum", "Snare Drum", "Tom Tom", "Cymbal", "Hi-Hat".
//  * nuta: F-number/blok -> Hz -> MIDI (OPLL: f = fnum * (clk/72) / 2^(19-blok); OPL2: clk/72, OPL3: clk/288,
//    f = fnum * sr / 2^(20-blok));
//  * glosnosc: OPLL - tlumienie 4-bitowe (15 - att) liniowo; OPL - TL operatora nosnego (63 - TL) liniowo
//    (przy polaczeniu addytywnym: mniejszy z TL obu operatorow); -> 0x00..0x40;
//  * instrument: OPLL - numer instrumentu z rejestru 0x30+ (1..15), patch uzytkownika (0) = 16 + numer
//    unikatowego zestawu rejestrow 0..7 (w kolejnosci pojawienia sie); OPL - numer unikatowego zestawu rejestrow
//    operatorow kanalu (TL nosnej pominiety) w kolejnosci pojawienia sie;
//  * perkusja (tryb rytmu): key-on bitu perkusji = nuta (wysokosc z kanalu 7/8/9 odpowiednio do instrumentu),
//    glosnosc z poziomu odpowiedniego operatora; kanaly melodyczne 7..9 sa w tym trybie wyciszone;
//  * OPL3 4-op: kanal parzysty (1..3, 10..12) niesie nute, kanal drugiej pary (4..6, 13..15) jest pusty;
//    glosnosc liczona z nosnych polaczenia (przyblizenie).
#include <array>

#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

// --- YM2413 ------------------------------------------------------------------------------------
class Opll final : public ChipDecoder {
public:
    Opll(Host& h, std::size_t first, std::uint32_t clock)
        : ChipDecoder(h, first), sr_(static_cast<double>(clock) / 72.0), custom_(16)
    {
    }

    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        if (reg <= 0x07) { patch_[reg] = val; return; }
        if (reg == 0x0E) { rhythmControl(val); return; }
        const std::size_t ch = reg & 0x0Fu;
        if (ch > 8) return;
        switch (reg & 0xF0) {
        case 0x10:
            fnumLo_[ch] = val;
            refresh(ch);
            break;
        case 0x20: {
            const bool wasKey = (hi_[ch] & 0x10) != 0;
            hi_[ch] = val;
            const bool key = (val & 0x10) != 0;
            if (key && !wasKey) keyOn(ch);
            else if (!key && wasKey) keyOff(ch);
            else refresh(ch);
            break;
        }
        case 0x30:
            iv_[ch] = val;
            refresh(ch);
            if (ch >= 6) refreshDrums();
            break;
        default: break;
        }
    }

private:
    [[nodiscard]] int midiOf(std::size_t ch) const
    {
        const unsigned fnum = fnumLo_[ch] | ((hi_[ch] & 1u) << 8);
        const int block = (hi_[ch] >> 1) & 7;
        return midiFromHzClamped(std::ldexp(fnum * sr_, block - 19));
    }
    [[nodiscard]] bool suppressed(std::size_t ch) const noexcept { return rhythm_ && ch >= 6; }
    [[nodiscard]] std::uint8_t volumeOf(std::size_t ch) const { return scaleVolume(15 - (iv_[ch] & 0x0F), 15); }

    void keyOn(std::size_t ch)
    {
        if (suppressed(ch)) return;
        std::uint8_t inst = iv_[ch] >> 4;
        if (inst == 0) {
            std::string key(reinterpret_cast<const char*>(patch_), 8);
            inst = custom_.id(key);
        }
        Tracked& t = mel_[ch];
        t = Tracked{true, midiOf(ch), inst, volumeOf(ch)};
        host_.rec.noteOn(base_ + ch, t.midi, t.inst, t.vol);
    }
    void keyOff(std::size_t ch)
    {
        if (mel_[ch].on) host_.rec.noteOff(base_ + ch);
        mel_[ch] = Tracked{};
    }
    void refresh(std::size_t ch)
    {
        Tracked& t = mel_[ch];
        if (t.on) {
            const int m = midiOf(ch);
            const std::uint8_t v = volumeOf(ch);
            if (m != t.midi) { t.midi = m; t.vol = v; host_.rec.noteOn(base_ + ch, m, t.inst, v); }
            else if (v != t.vol) { t.vol = v; host_.rec.volume(base_ + ch, v); }
        }
        if (ch >= 6) refreshDrums();
    }

    // Perkusja: indeks 0..4 = BD, SD, TOM, CYM, HH; bit klawisza w rejestrze 0x0E: 4,3,2,1,0.
    [[nodiscard]] static constexpr unsigned drumBit(unsigned d) noexcept { return 4u - d; }
    [[nodiscard]] int drumMidi(unsigned d) const
    {
        static constexpr std::uint8_t kCh[5] = {6, 7, 8, 8, 7};
        return midiOf(kCh[d]);
    }
    [[nodiscard]] std::uint8_t drumVolume(unsigned d) const
    {
        switch (d) {
        case 0: return scaleVolume(15 - (iv_[6] & 0x0F), 15);
        case 1: return scaleVolume(15 - (iv_[7] & 0x0F), 15);
        case 2: return scaleVolume(15 - (iv_[8] >> 4), 15);
        case 3: return scaleVolume(15 - (iv_[8] & 0x0F), 15);
        default: return scaleVolume(15 - (iv_[7] >> 4), 15);
        }
    }
    void refreshDrums()
    {
        for (unsigned d = 0; d < 5; ++d) {
            Tracked& t = drum_[d];
            if (!t.on) continue;
            const std::uint8_t v = drumVolume(d);
            if (v != t.vol) { t.vol = v; host_.rec.volume(base_ + 9 + d, v); }
        }
    }
    void rhythmControl(std::uint8_t val)
    {
        const bool newRhythm = (val & 0x20) != 0;
        const std::uint8_t keys = newRhythm ? (val & 0x1F) : 0;
        if (newRhythm && !rhythm_) {
            for (std::size_t ch = 6; ch < 9; ++ch) keyOff(ch);   // kanaly melodyczne 7..9 przechodza na perkusje
        }
        rhythm_ = newRhythm;
        for (unsigned d = 0; d < 5; ++d) {
            const bool now = (keys >> drumBit(d)) & 1u;
            Tracked& t = drum_[d];
            if (now && !t.on) {
                t = Tracked{true, drumMidi(d), 0, drumVolume(d)};
                host_.rec.noteOn(base_ + 9 + d, t.midi, 0, t.vol);
            } else if (!now && t.on) {
                host_.rec.noteOff(base_ + 9 + d);
                t = Tracked{};
            }
        }
    }

    double sr_;
    InstrumentBank custom_;
    std::uint8_t patch_[8] = {};
    std::uint8_t fnumLo_[9] = {};
    std::uint8_t hi_[9] = {};
    std::uint8_t iv_[9] = {};
    bool rhythm_ = false;
    Tracked mel_[9];
    Tracked drum_[5];
};

// --- YM3812 / YM3526 / YMF262 -----------------------------------------------------------------
class Opl final : public ChipDecoder {
public:
    Opl(Host& h, std::size_t first, std::uint32_t clock, bool opl3)
        : ChipDecoder(h, first), opl3_(opl3), nMel_(opl3 ? 18u : 9u),
          sr_(static_cast<double>(clock) / (opl3 ? 288.0 : 72.0))
    {
    }

    void write(std::uint8_t port, std::uint8_t reg, std::uint8_t val) override
    {
        const unsigned bank = (opl3_ && port) ? 1u : 0u;
        if (reg == 0x04 && bank == 1) { fourOp_ = val & 0x3F; return; }
        if (reg == 0xBD && bank == 0) { rhythmControl(val); return; }
        if (reg >= 0xA0 && reg <= 0xC8) {
            const unsigned c = reg & 0x0Fu;
            if (c > 8) return;
            const std::size_t ch = bank * 9u + c;
            switch (reg & 0xF0) {
            case 0xA0: fnumLo_[ch] = val; touch(ch); break;
            case 0xB0: {
                const bool was = (b0_[ch] & 0x20) != 0;
                b0_[ch] = val;
                const bool key = (val & 0x20) != 0;
                if (key && !was) keyOn(ch);
                else if (!key && was) keyOff(ch);
                else touch(ch);
                break;
            }
            case 0xC0: c0_[ch] = val; touch(ch); break;
            default: break;
            }
            return;
        }
        const unsigned grpBase = reg & 0xE0u;
        if (grpBase != 0x20 && grpBase != 0x40 && grpBase != 0x60 && grpBase != 0x80 && grpBase != 0xE0) return;
        const unsigned o = reg & 0x1Fu;
        if (o > 0x15) return;
        const unsigned low = o & 7u, hi = o >> 3;
        if (low > 5 || hi > 2) return;
        const unsigned slot = hi * 6 + low;
        const unsigned group = grpBase == 0xE0 ? 4u : (grpBase - 0x20u) >> 5;
        ops_[bank * 18 + slot][group] = val;
        // kanal wlasciciel operatora
        const std::size_t ch = bank * 9u + (slot / 6) * 3 + slot % 3;
        touch(ch);
    }

private:
    using OpRegs = std::array<std::uint8_t, 5>;   // 0x20, 0x40, 0x60, 0x80, 0xE0
    static constexpr unsigned kTl = 1;

    [[nodiscard]] const OpRegs& opOf(std::size_t ch, bool carrier) const
    {
        const unsigned bank = static_cast<unsigned>(ch / 9), c = static_cast<unsigned>(ch % 9);
        const unsigned slot = (c / 3) * 6 + c % 3 + (carrier ? 3u : 0u);
        return ops_[bank * 18 + slot];
    }
    [[nodiscard]] bool is4opFirst(std::size_t ch) const noexcept
    {
        if (!opl3_) return false;
        if (ch < 3) return (fourOp_ >> ch) & 1u;
        if (ch >= 9 && ch < 12) return (fourOp_ >> (3 + ch - 9)) & 1u;
        return false;
    }
    [[nodiscard]] bool is4opSecond(std::size_t ch) const noexcept
    {
        if (!opl3_) return false;
        if (ch >= 3 && ch < 6) return (fourOp_ >> (ch - 3)) & 1u;
        if (ch >= 12 && ch < 15) return (fourOp_ >> (3 + ch - 12)) & 1u;
        return false;
    }
    [[nodiscard]] bool suppressed(std::size_t ch) const noexcept
    {
        return is4opSecond(ch) || (rhythm_ && ch >= 6 && ch < 9);
    }
    [[nodiscard]] int midiOf(std::size_t ch) const
    {
        const unsigned fnum = fnumLo_[ch] | ((b0_[ch] & 3u) << 8);
        const int block = (b0_[ch] >> 2) & 7;
        return midiFromHzClamped(std::ldexp(fnum * sr_, block - 20));
    }
    [[nodiscard]] static int tl(const OpRegs& o) noexcept { return o[kTl] & 0x3F; }
    [[nodiscard]] std::uint8_t volumeOf(std::size_t ch) const
    {
        int minTl;
        if (is4opFirst(ch)) {
            const bool cA = c0_[ch] & 1, cB = c0_[ch + 3] & 1;
            minTl = tl(opOf(ch + 3, true));
            if (cB) minTl = std::min(minTl, tl(opOf(ch + 3, false)));
            if (cA) minTl = std::min(minTl, tl(opOf(ch, true)));
        } else {
            minTl = tl(opOf(ch, true));
            if (c0_[ch] & 1) minTl = std::min(minTl, tl(opOf(ch, false)));
        }
        return scaleVolume(63 - minTl, 63);
    }
    [[nodiscard]] std::string patchKey(std::size_t ch) const
    {
        std::string k;
        auto addCh = [&](std::size_t c) {
            const bool additive = (c0_[c] & 1) != 0;
            for (int car = 0; car < 2; ++car) {
                OpRegs o = opOf(c, car != 0);
                if (car || additive) o[kTl] = static_cast<std::uint8_t>(o[kTl] & 0xC0);   // TL nosnej = glosnosc
                for (std::uint8_t b : o) k.push_back(static_cast<char>(b));
            }
            k.push_back(static_cast<char>(c0_[c] & 0x0F));
        };
        addCh(ch);
        if (is4opFirst(ch)) addCh(ch + 3);
        return k;
    }

    void keyOn(std::size_t ch)
    {
        if (suppressed(ch)) return;
        Tracked& t = mel_[ch];
        t = Tracked{true, midiOf(ch), bank_.id(patchKey(ch)), volumeOf(ch)};
        host_.rec.noteOn(base_ + ch, t.midi, t.inst, t.vol);
    }
    void keyOff(std::size_t ch)
    {
        if (mel_[ch].on) host_.rec.noteOff(base_ + ch);
        mel_[ch] = Tracked{};
    }
    // Zmiana rejestrow kanalu przy granej nucie: nowa nuta (ton) albo tylko glosnosc.
    void touch(std::size_t ch)
    {
        if (is4opSecond(ch)) ch -= 3;   // glosnosc pary 4-op zalezy tez od drugiego kanalu
        Tracked& t = mel_[ch];
        if (t.on) {
            const int m = midiOf(ch);
            const std::uint8_t v = volumeOf(ch);
            if (m != t.midi) { t.midi = m; t.vol = v; host_.rec.noteOn(base_ + ch, m, t.inst, v); }
            else if (v != t.vol) { t.vol = v; host_.rec.volume(base_ + ch, v); }
        }
        if (ch >= 6 && ch < 9) refreshDrums();
    }

    // Perkusja: BD, SD, TOM, CYM, HH <-> bity 0xBD: 4, 3, 2, 1, 0.
    [[nodiscard]] static constexpr unsigned drumBit(unsigned d) noexcept { return 4u - d; }
    [[nodiscard]] int drumMidi(unsigned d) const
    {
        static constexpr std::uint8_t kCh[5] = {6, 7, 8, 8, 7};
        return midiOf(kCh[d]);
    }
    [[nodiscard]] std::uint8_t drumVolume(unsigned d) const
    {
        switch (d) {
        case 0: return scaleVolume(63 - tl(opOf(6, true)), 63);    // BD: nosna kanalu 7
        case 1: return scaleVolume(63 - tl(opOf(7, true)), 63);    // SD: nosna kanalu 8
        case 2: return scaleVolume(63 - tl(opOf(8, false)), 63);   // TOM: modulator kanalu 9
        case 3: return scaleVolume(63 - tl(opOf(8, true)), 63);    // CYM: nosna kanalu 9
        default: return scaleVolume(63 - tl(opOf(7, false)), 63);  // HH: modulator kanalu 8
        }
    }
    void refreshDrums()
    {
        for (unsigned d = 0; d < 5; ++d) {
            Tracked& t = drum_[d];
            if (!t.on) continue;
            const std::uint8_t v = drumVolume(d);
            if (v != t.vol) { t.vol = v; host_.rec.volume(base_ + nMel_ + d, v); }
        }
    }
    void rhythmControl(std::uint8_t val)
    {
        const bool newRhythm = (val & 0x20) != 0;
        const std::uint8_t keys = newRhythm ? (val & 0x1F) : 0;
        if (newRhythm && !rhythm_) {
            for (std::size_t ch = 6; ch < 9; ++ch) keyOff(ch);
        }
        rhythm_ = newRhythm;
        for (unsigned d = 0; d < 5; ++d) {
            const bool now = (keys >> drumBit(d)) & 1u;
            Tracked& t = drum_[d];
            if (now && !t.on) {
                t = Tracked{true, drumMidi(d), 0, drumVolume(d)};
                host_.rec.noteOn(base_ + nMel_ + d, t.midi, 0, t.vol);
            } else if (!now && t.on) {
                host_.rec.noteOff(base_ + nMel_ + d);
                t = Tracked{};
            }
        }
    }

    bool opl3_;
    std::size_t nMel_;
    double sr_;
    InstrumentBank bank_;
    std::array<OpRegs, 36> ops_{};
    std::uint8_t fnumLo_[18] = {};
    std::uint8_t b0_[18] = {};
    std::uint8_t c0_[18] = {};
    std::uint8_t fourOp_ = 0;
    bool rhythm_ = false;
    Tracked mel_[18];
    Tracked drum_[5];
};

} // namespace

std::unique_ptr<ChipDecoder> makeYm2413(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<Opll>(h, first, clockHz);
}
std::unique_ptr<ChipDecoder> makeOpl(Host& h, std::size_t first, std::uint32_t clockHz, bool opl3)
{
    return std::make_unique<Opl>(h, first, clockHz, opl3);
}

} // namespace vgmplay::chipstate::detail
