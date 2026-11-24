// vgmplay - modul chipstate: dekodery PSG - SN76489/SN76496 oraz AY-3-8910 (rdzen wspoldzielony z SSG YM2203).
// Uklady bez key-on: kanal "gra", gdy tlumienie < maks. (SN) / amplituda > 0 (AY) i ton jest w zakresie MIDI;
// zdarzenia wynikaja z roznicy miedzy stanem zadanym a ostatnio zgloszonym (emitState).
#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

// --- SN76489 -----------------------------------------------------------------------------------
// Instrument: 0 = ton, 1 = szum bialy, 2 = szum periodyczny (kanal "Noise").
// Nuta kanalu szumu: czestotliwosc zegara przesuwania rejestru dla wybranej szybkosci (clock/(32*p), p = 16/32/64
// albo okres tonu kanalu 3) - to samo "przeliczenie na nute" co dla tonu, wiec rozne szybkosci szumu daja rozne nuty.
// Glosnosc: (15 - tlumienie) liniowo -> 0..0x40 (tlumienie 15 = wylaczony).
class Sn76489 final : public ChipDecoder {
public:
    Sn76489(Host& h, std::size_t first, double clock, bool fastClockDiv)
        : ChipDecoder(h, first), clock_(clock), div_(fastClockDiv ? 4.0 : 32.0)
    {
        for (auto& a : att_) a = 15;
    }

    void write(std::uint8_t /*port*/, std::uint8_t /*reg*/, std::uint8_t val) override
    {
        if (val & 0x80) {
            latchCh_ = (val >> 5) & 3;
            latchVol_ = (val & 0x10) != 0;
            data(val & 0x0F, true);
        } else {
            data(val, false);
        }
    }

private:
    void data(std::uint8_t v, bool lowNibbleOnly)
    {
        const int ch = latchCh_;
        if (latchVol_) {
            att_[ch] = v & 0x0F;
            update(ch, false);
            return;
        }
        if (ch < 3) {
            if (lowNibbleOnly) period_[ch] = static_cast<std::uint16_t>((period_[ch] & 0x3F0) | (v & 0x0F));
            else period_[ch] = static_cast<std::uint16_t>((period_[ch] & 0x00F) | ((v & 0x3F) << 4));
            update(ch, false);
            if (ch == 2 && (noise_ & 3) == 3) update(3, false);   // szum "z kanalu 3"
        } else {
            noise_ = v & 7;
            update(3, true);   // zapis rejestru szumu resetuje LFSR - kazdy zapis to nowe uderzenie
        }
    }

    void update(int ch, bool retrigger)
    {
        const bool audible = att_[ch] < 15;
        const std::uint8_t vol = scaleVolume(15 - att_[ch], 15);
        int midi;
        std::uint8_t inst = 0;
        if (ch < 3) {
            const unsigned p = period_[ch] ? period_[ch] : 0x400u;
            midi = midiFromHz(clock_ / (div_ * p));
        } else {
            const unsigned r = noise_ & 3;
            const unsigned p = r < 3 ? (16u << r) : (period_[2] ? period_[2] : 0x400u);
            midi = midiFromHz(clock_ / (div_ * p));
            if (midi < 0 && audible) midi = 127;
            inst = (noise_ & 4) ? 1 : 2;
        }
        emitState(host_.rec, base_ + static_cast<std::size_t>(ch), tr_[ch], audible, midi, inst, vol,
                  retrigger && audible);
    }

    double clock_;
    double div_;
    int latchCh_ = 0;
    bool latchVol_ = false;
    std::uint16_t period_[3] = {};
    std::uint8_t att_[4] = {};
    std::uint8_t noise_ = 0;
    Tracked tr_[4];
};

// --- AY-3-8910 ---------------------------------------------------------------------------------
class AyDecoder final : public ChipDecoder {
public:
    AyDecoder(Host& h, std::size_t first, double clock) : ChipDecoder(h, first), core_(h, first, clock) {}
    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override { core_.write(reg & 0x0F, val); }

private:
    AyCore core_;
};

} // namespace

// Instrument AY: bit0 = szum wmieszany, bit1 = obwiednia amplitudy, bit2 = ton wylaczony (sam szum) -> 0..7.
// Nuta: z okresu tonu (f = clock / (16 * okres)); przy samym szumie z okresu szumu (obcieta do 0..127).
// Glosnosc: amplituda 0..15 liniowo -> 0..0x40; tryb obwiedni = 0x40 (ksztalt obwiedni nie jest sledzony).
void AyCore::write(std::uint8_t reg, std::uint8_t val)
{
    if (reg > 15) return;
    r_[reg] = val;
    if (reg <= 10) {
        for (int c = 0; c < 3; ++c) update(c);
    }
}

void AyCore::update(int c)
{
    const bool toneOn = !(r_[7] & (1u << c));
    const bool noiseOn = !(r_[7] & (8u << c));
    const std::uint8_t v = r_[8 + c];
    const bool env = (v & 0x10) != 0;
    const int amp = v & 0x0F;
    const bool audible = (toneOn || noiseOn) && (env || amp > 0);
    int midi;
    if (toneOn) {
        unsigned p = static_cast<unsigned>(r_[2 * c]) | (static_cast<unsigned>(r_[2 * c + 1] & 0x0F) << 8);
        if (p == 0) p = 1;
        midi = midiFromHz(clock_ / (16.0 * p));
    } else {
        unsigned p = r_[6] & 0x1F;
        if (p == 0) p = 1;
        midi = midiFromHzClamped(clock_ / (16.0 * p));
    }
    const std::uint8_t inst = static_cast<std::uint8_t>((noiseOn ? 1 : 0) | (env ? 2 : 0) | (toneOn ? 0 : 4));
    const std::uint8_t vol = env ? std::uint8_t{0x40} : scaleVolume(amp, 15);
    emitState(host_.rec, base_ + static_cast<std::size_t>(c), tr_[c], audible, midi, inst, vol);
}

std::unique_ptr<ChipDecoder> makeSn76489(Host& h, std::size_t first, std::uint32_t clockHz, std::uint32_t flags)
{
    // flags bit 27 = bit 3 bajtu 0x2B naglowka (clkDiv: 1 = SN76494/SN94624, 0 = dzielnik 8).
    return std::make_unique<Sn76489>(h, first, static_cast<double>(clockHz), (flags & (1u << 27)) != 0);
}

std::unique_ptr<ChipDecoder> makeAy8910(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<AyDecoder>(h, first, static_cast<double>(clockHz));
}

} // namespace vgmplay::chipstate::detail
