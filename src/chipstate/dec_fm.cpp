// vgmplay - modul chipstate: dekodery rodziny OPN/OPM - YM2612 (+DAC), YM2203 (FM + SSG), YM2151.
//
// Wspolne zasady (FM 4-operatorowe):
//  * key-on = przejscie maski operatorow z 0 na niezerowa; key-off = z niezerowej na 0; zmiana tonu przy
//    graniu daje nowa nute (portamento), zmiana TL nosnych - tylko glosnosc w wierszu; dolaczenie operatorow
//    do maski przy granej nucie (nowe zbocze key-on operatora = ponowny atak) daje nute o tym samym tonie
//    (retrigger); powtorzenie tej samej maski nie jest zboczem i nic nie zmienia;
//  * glosnosc = (127 - min TL operatorow NOSNYCH wg algorytmu) liniowo -> 0x00..0x40;
//  * instrument = numer unikatowego zestawu rejestrow operatorow + FB/ALG nadawany w kolejnosci pojawienia sie
//    (od 0, osobno dla kazdej instancji ukladu); TL operatorow nosnych jest POMINIETY w kluczu (to glosnosc),
//    TL modulatorow - wlaczony. Instrument jest wyznaczany w chwili key-on i nie zmienia sie w trakcie nuty.
#include <array>
#include <cstring>

#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

// Kolejnosc slotow w adresach rejestrow (S1, S3, S2, S4) -> operator logiczny op1..op4 (0..3).
constexpr std::uint8_t kSlotToOp[4] = {0, 2, 1, 3};
// Operatory nosne dla algorytmow 0..7 (bit i = op(i+1)).
constexpr std::uint8_t kCarrierMask[8] = {0x8, 0x8, 0x8, 0x8, 0xA, 0xE, 0xE, 0xF};

struct FmChan {
    // [0]=DT/MUL [1]=TL [2]=KS/AR [3]=AM/D1R [4]=D2R [5]=D1L/RR [6]=SSG-EG (OPM: DT2/D2R w [4], brak SSG)
    std::uint8_t op[4][7] = {};
    std::uint8_t fbAlg = 0;      // bity 0..5 (ALG 0..2, FB 3..5); bity panoramy odciete
    std::uint8_t keyMask = 0;
    Tracked tr;

    [[nodiscard]] std::uint8_t volume() const noexcept
    {
        const std::uint8_t mask = kCarrierMask[fbAlg & 7];
        int minTl = 127;
        for (int i = 0; i < 4; ++i)
            if (mask & (1u << i)) minTl = std::min<int>(minTl, op[i][1] & 0x7F);
        return scaleVolume(127 - minTl, 127);
    }
    [[nodiscard]] std::string patchKey() const
    {
        std::string k;
        k.reserve(29);
        k.push_back(static_cast<char>(fbAlg));
        const std::uint8_t mask = kCarrierMask[fbAlg & 7];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 7; ++j) {
                const bool skip = (j == 1) && (mask & (1u << i));
                k.push_back(static_cast<char>(skip ? 0 : op[i][j]));
            }
        }
        return k;
    }
};

// Wspolna logika key-on / key-off / aktualizacji tonu i glosnosci kanalu FM.
class FmVoiceOps {
public:
    FmVoiceOps(Host& h) : fhost_(h) {}

protected:
    void keyOn(std::size_t ch, FmChan& c, int midi, bool suppressed, std::size_t chanIndex)
    {
        (void)ch;
        if (suppressed) return;
        c.tr.inst = bank_.id(c.patchKey());
        c.tr.midi = midi;
        c.tr.vol = c.volume();
        c.tr.on = true;
        fhost_.rec.noteOn(chanIndex, midi, c.tr.inst, c.tr.vol);
    }
    void keyOff(FmChan& c, std::size_t chanIndex)
    {
        if (c.tr.on) fhost_.rec.noteOff(chanIndex);
        c.tr.on = false;
    }
    // Nowe zbocze key-on operatora przy granej nucie (maska zyskala bity): ponowny atak tej samej nuty.
    void retrigger(FmChan& c, int midi, std::size_t chanIndex)
    {
        if (!c.tr.on) return;
        c.tr.midi = midi;
        c.tr.vol = c.volume();
        fhost_.rec.noteOn(chanIndex, midi, c.tr.inst, c.tr.vol);
    }
    // Zmiana tonu / glosnosci przy granej nucie.
    void refresh(FmChan& c, int midi, std::size_t chanIndex)
    {
        if (!c.tr.on) return;
        const std::uint8_t vol = c.volume();
        if (midi != c.tr.midi) {
            c.tr.midi = midi;
            c.tr.vol = vol;
            fhost_.rec.noteOn(chanIndex, midi, c.tr.inst, vol);
        } else if (vol != c.tr.vol) {
            c.tr.vol = vol;
            fhost_.rec.volume(chanIndex, vol);
        }
    }

    Host& fhost_;
    InstrumentBank bank_;
};

// --- YM2612 / YM2203 --------------------------------------------------------------------------
class Opn final : public ChipDecoder, private FmVoiceOps {
public:
    enum class Kind { Ym2612, Ym2203 };

    Opn(Host& h, std::size_t first, std::uint32_t clock, Kind kind)
        : ChipDecoder(h, first), FmVoiceOps(h), clock_(clock), kind_(kind),
          nCh_(kind == Kind::Ym2612 ? 6u : 3u)
    {
        if (kind_ == Kind::Ym2203) {
            ssg_ = std::make_unique<AyCore>(h, first + 3, ssgClock());
        }
    }

    void write(std::uint8_t port, std::uint8_t reg, std::uint8_t val) override
    {
        if (kind_ == Kind::Ym2203) {
            if (port != 0) return;
            if (reg < 0x10) { ssg_->write(reg, val); return; }
            if (reg >= 0x2D && reg <= 0x2F) { prescaler(reg); return; }
        }
        if (reg < 0x30) {
            if (port == 0) system(reg, val);
            return;
        }
        const unsigned lane = reg & 3u;
        if (lane == 3) return;
        if (port == 0 && (reg & 0xFC) == 0xA8) { ch3Freq(lane, val, false); return; }   // CH3 special: F-num operatorow
        if (port == 0 && (reg & 0xFC) == 0xAC) { ch3Freq(lane, val, true); return; }
        const std::size_t ch = static_cast<std::size_t>(port) * 3 + lane;
        if (ch >= nCh_) return;
        FmChan& c = chan_[ch];
        if (reg < 0xA0) {
            const unsigned group = (static_cast<unsigned>(reg) - 0x30) >> 4;   // 0..6
            const unsigned slot = (reg >> 2) & 3u;
            c.op[kSlotToOp[slot]][group] = val;
            if (group == 1) refresh(c, midiOf(ch), base_ + ch);   // TL -> glosnosc
            return;
        }
        switch (reg & 0xFC) {
        case 0xA0:   // F-number (low): zatwierdza zatrzask z 0xA4
            fnum_[ch] = static_cast<std::uint16_t>(((latch_[ch] & 7u) << 8) | val);
            blk_[ch] = static_cast<std::uint8_t>((latch_[ch] >> 3) & 7u);
            refresh(c, midiOf(ch), base_ + ch);
            break;
        case 0xA4: latch_[ch] = val & 0x3F; break;
        case 0xB0:
            c.fbAlg = val & 0x3F;
            refresh(c, midiOf(ch), base_ + ch);   // algorytm zmienia zbior nosnych
            break;
        default: break;
        }
    }

    void dacData() override
    {
        if (kind_ != Kind::Ym2612 || !dacEn_) return;
        if (!dac_.on) startDac(dacInst_);
        if (!dacStream_) { dacDeadline_ = host_.now + kDacIdleTicks; }
    }
    void dacStreamStart(std::uint8_t instrument, std::uint64_t endTick) override
    {
        if (kind_ != Kind::Ym2612) return;
        dacInst_ = instrument;
        dacStream_ = true;
        dacDeadline_ = endTick;
        if (dacEn_) startDac(instrument);
    }
    void dacStreamStop() override
    {
        if (kind_ != Kind::Ym2612) return;
        stopDac();
    }
    [[nodiscard]] std::uint64_t deadline() const override { return dacDeadline_; }
    void onDeadline(std::uint64_t when) override
    {
        if (when >= dacDeadline_) stopDac();
    }

private:
    // Cisza DAC: brak zapisu probki dluzej niz ~2 klatki (1470 tickow) konczy "nute" DAC.
    static constexpr std::uint64_t kDacIdleTicks = 1470;

    void startDac(std::uint8_t inst)
    {
        // Nuta DAC = C-4 (60), glosnosc maks.: probka nie ma wysokosci tonu, instrument = numer bloku/probki.
        host_.rec.noteOn(base_ + 6, 60, inst, 0x40);
        dac_ = Tracked{true, 60, inst, 0x40};
    }
    void stopDac()
    {
        if (dac_.on) host_.rec.noteOff(base_ + 6);
        dac_ = Tracked{};
        dacStream_ = false;
        dacDeadline_ = kNever;
    }

    [[nodiscard]] double fmSampleRate() const
    {
        if (kind_ == Kind::Ym2612) return clock_ / 144.0;
        static constexpr double kPres[4] = {24, 24, 72, 36};
        return clock_ / kPres[sel_ & 3];
    }
    [[nodiscard]] double ssgClock() const
    {
        static constexpr double kSsgPres[4] = {1, 1, 4, 2};
        return clock_ * 2.0 / kSsgPres[sel_ & 3];
    }
    [[nodiscard]] int midiOf(std::size_t ch) const
    {
        std::uint16_t fn = fnum_[ch];
        std::uint8_t blk = blk_[ch];
        // CH3 special/CSM (reg 0x27 bity 6-7): osobne F-number operatorow w 0xA8-0xAE. Ton kanalu = F-number
        // z 0xA2/0xA6 (operator S4), a gdy ich nie zapisano - pierwszy zapisany F-number operatora.
        if (ch == 2 && ch3Mode_ != 0 && fn == 0) {
            for (unsigned i = 0; i < 3; ++i)
                if (opFnum_[i] != 0) { fn = opFnum_[i]; blk = opBlk_[i]; break; }
        }
        return midiFromHzClamped(std::ldexp(static_cast<double>(fn) * fmSampleRate(), blk - 21));
    }
    // 0xA8-0xAA = F-number (low) operatora, 0xAC-0xAE = zatrzask (blok + F-number high); zapis low zatwierdza.
    void ch3Freq(unsigned lane, std::uint8_t val, bool latch)
    {
        if (latch) { opLatch_[lane] = val & 0x3F; return; }
        opFnum_[lane] = static_cast<std::uint16_t>(((opLatch_[lane] & 7u) << 8) | val);
        opBlk_[lane] = static_cast<std::uint8_t>((opLatch_[lane] >> 3) & 7u);
        if (ch3Mode_ != 0) refresh(chan_[2], midiOf(2), base_ + 2);
    }

    void prescaler(std::uint8_t reg)
    {
        if (reg == 0x2D) sel_ |= 2;
        else if (reg == 0x2E) sel_ |= 1;
        else sel_ = 0;
        if (ssg_) ssg_->setClock(ssgClock());
    }

    void system(std::uint8_t reg, std::uint8_t val)
    {
        switch (reg) {
        case 0x28: {
            unsigned ch = val & 7u;
            if (kind_ == Kind::Ym2612) { if (ch >= 7 || ch == 3) return; if (ch > 3) --ch; }
            else if (ch >= 3) return;
            FmChan& c = chan_[ch];
            const std::uint8_t mask = val >> 4;
            const bool wasOn = c.keyMask != 0;
            const std::uint8_t oldMask = c.keyMask;
            c.keyMask = mask;
            const bool suppressed = (kind_ == Kind::Ym2612 && ch == 5 && dacEn_);
            if (mask && !wasOn) keyOn(ch, c, midiOf(ch), suppressed, base_ + ch);
            else if (!mask && wasOn) keyOff(c, base_ + ch);
            else if (mask & ~oldMask) retrigger(c, midiOf(ch), base_ + ch);
            break;
        }
        case 0x27:
            ch3Mode_ = val >> 6;
            if (chan_[2].tr.on) refresh(chan_[2], midiOf(2), base_ + 2);
            break;
        case 0x2A:
            if (kind_ == Kind::Ym2612) dacData();
            break;
        case 0x2B:
            if (kind_ != Kind::Ym2612) break;
            dacEn_ = (val & 0x80) != 0;
            if (dacEn_) keyOff(chan_[5], base_ + 5);   // FM 6 zastapiony przez DAC
            else stopDac();
            break;
        default: break;
        }
    }

    std::uint32_t clock_;
    Kind kind_;
    unsigned nCh_;
    unsigned sel_ = 2;   // YM2203: domyslny preskaler FM /6, SSG /4 (reg 0x2D/0x2E/0x2F go zmieniaja)
    std::array<FmChan, 6> chan_{};
    std::uint16_t fnum_[6] = {};
    std::uint8_t blk_[6] = {};
    std::uint8_t latch_[6] = {};
    std::uint8_t ch3Mode_ = 0;                // reg 0x27 bity 6-7: 0 normalny, != 0 CH3 special / CSM
    std::uint16_t opFnum_[3] = {};            // CH3 special: F-number operatorow (0xA8-0xAA)
    std::uint8_t opBlk_[3] = {};
    std::uint8_t opLatch_[3] = {};            // 0xAC-0xAE
    std::unique_ptr<AyCore> ssg_;
    bool dacEn_ = false;
    bool dacStream_ = false;
    std::uint8_t dacInst_ = 0;
    Tracked dac_;
    std::uint64_t dacDeadline_ = kNever;
};

// --- YM2151 (OPM) -----------------------------------------------------------------------------
class Opm final : public ChipDecoder, private FmVoiceOps {
public:
    Opm(Host& h, std::size_t first, std::uint32_t clock) : ChipDecoder(h, first), FmVoiceOps(h), clock_(clock) {}

    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        if (reg == 0x08) {
            const std::size_t ch = val & 7u;
            FmChan& c = chan_[ch];
            const std::uint8_t mask = (val >> 3) & 0x0F;
            const bool wasOn = c.keyMask != 0;
            const std::uint8_t oldMask = c.keyMask;
            c.keyMask = mask;
            if (mask && !wasOn) keyOn(ch, c, midiOf(ch), false, base_ + ch);
            else if (!mask && wasOn) keyOff(c, base_ + ch);
            else if (mask & ~oldMask) retrigger(c, midiOf(ch), base_ + ch);
            return;
        }
        if (reg < 0x20) return;
        const std::size_t ch = reg & 7u;
        FmChan& c = chan_[ch];
        if (reg < 0x28) { c.fbAlg = val & 0x3F; refresh(c, midiOf(ch), base_ + ch); return; }
        if (reg < 0x30) { kc_[ch] = val; refresh(c, midiOf(ch), base_ + ch); return; }
        if (reg < 0x38) { kf_[ch] = val >> 2; refresh(c, midiOf(ch), base_ + ch); return; }
        if (reg < 0x40) return;   // PMS/AMS
        const unsigned group = (static_cast<unsigned>(reg) - 0x40) >> 5;   // 0..5: DT1/MUL, TL, KS/AR, AMS/D1R, DT2/D2R, D1L/RR
        const unsigned slot = (reg >> 3) & 3u;
        c.op[kSlotToOp[slot]][group] = val;
        if (group == 1) refresh(c, midiOf(ch), base_ + ch);
    }

private:
    // KC: bity 4..6 = oktawa, 0..3 = kod nuty (0,1,2,4,5,6,8,9,10,12,13,14 = C#,D,D#,E,F,F#,G,G#,A,A#,B,C
    // nastepnej oktawy); KF: 1/64 polotonu. A4 (440 Hz) = KC 0x4A przy zegarze 3579545 Hz; inny zegar
    // przesuwa wysokosc o 12*log2(clock/3579545).
    [[nodiscard]] int midiOf(std::size_t ch) const
    {
        static constexpr std::uint8_t kSemi[16] = {1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10, 11, 12, 12};
        const unsigned oct = (kc_[ch] >> 4) & 7u;
        const double p = 12.0 * (oct + 1) + kSemi[kc_[ch] & 0x0F] + kf_[ch] / 64.0 +
                         12.0 * std::log2(static_cast<double>(clock_) / 3579545.0);
        return static_cast<int>(std::clamp(std::round(p), 0.0, 127.0));
    }

    std::uint32_t clock_;
    std::array<FmChan, 8> chan_{};
    std::uint8_t kc_[8] = {};
    std::uint8_t kf_[8] = {};
};

} // namespace

std::unique_ptr<ChipDecoder> makeYm2612(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<Opn>(h, first, clockHz, Opn::Kind::Ym2612);
}
std::unique_ptr<ChipDecoder> makeYm2203(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<Opn>(h, first, clockHz, Opn::Kind::Ym2203);
}
std::unique_ptr<ChipDecoder> makeYm2151(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<Opm>(h, first, clockHz);
}

} // namespace vgmplay::chipstate::detail
