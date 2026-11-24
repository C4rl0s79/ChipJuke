// vgmplay - modul chipstate: NES APU (2A03: 2 x pulse, triangle, noise, DPCM) + opcjonalnie FDS.
//
// Rejestry (komenda 0xB4; pattern.cpp mapuje je jak libvgm): 0x00..0x1F = $4000..$401F, 0x23 = $4023 (FDS I/O),
// 0x40..0x7F = $4040..$407F (tablica fali FDS), 0x80..0x8A = $4080..$408A.
// Kanaly (kolejnosc jak w ChannelLayout): Square 1, Square 2, Triangle, Noise, DPCM, [FDS].
//
// Nuta:
//  * Square: f = clock / (16 * (timer + 1)); instrument = wypelnienie (duty 0..3); glosnosc = stala glosnosc 0..15
//    -> 0..0x40, tryb obwiedni sprzetowej = 0x40; wyciszenie: timer < 8, sweep (cel >= 0x800), licznik dlugosci = 0,
//    glosnosc 0 / wygasla obwiednia.
//  * Triangle: f = clock / (32 * (timer + 1)); glosnosc 0x40; wyciszenie: licznik liniowy / dlugosci = 0, timer = 0.
//  * Noise: nuta z czestotliwosci zegara rejestru przesuwnego f = clock / (32 * okres) (okres z tabeli 16 pozycji;
//    to samo przeliczenie co szum SN76489), instrument 1 = szum dlugi, 2 = szum krotki (bit 7 $400E).
//  * DPCM: start z $4015 bit 4 (adres $C000 + 64*$4012, dlugosc 16*$4013+1 bajtow); instrument = numer unikatowej
//    probki (adres, dlugosc) w kolejnosci pojawienia sie; nuta z szybkosci odczytu: C-4 (60) = najszybsza (indeks 15,
//    33143 Hz), kazda nizsza pozycja o odpowiednia liczbe poltonow nizej; key-off po wyczerpaniu bajtow (bez petli).
//  * FDS: f = rejestr(12 bit) * clock / 2^22, instrument = numer unikatowej tablicy fali (64 bajty), glosnosc z $4080
//    (tryb bez obwiedni) pomnozona przez glosnosc glowna $4089; tryb z obwiednia = 0x40 (x glosnosc glowna).
// Key-off: licznik dlugosci (tablica NES, krok 120 Hz = polowa klatki 7457 cykli), licznik liniowy trojkata
// (240 Hz), wygasniecie obwiedni sprzetowej (bez petli), wyzerowanie bitu w $4015, glosnosc 0.
// Ograniczenia: sweep NIE zmienia wysokosci w czasie (tylko wycisza przy przepelnieniu), modulacja FDS jest pomijana,
// brak $4011 (bezposrednie zapisy DAC), czasy licznikow to model idealny (bez fazy wzgledem klatki).
#include <cstdio>

#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

constexpr std::uint8_t kLengthTable[32] = {0x0A, 0xFE, 0x14, 0x02, 0x28, 0x04, 0x50, 0x06, 0xA0, 0x08, 0x3C,
                                           0x0A, 0x0E, 0x0C, 0x1A, 0x0E, 0x0C, 0x10, 0x18, 0x12, 0x30, 0x14,
                                           0x60, 0x16, 0xC0, 0x18, 0x48, 0x1A, 0x10, 0x1C, 0x20, 0x1E};
constexpr unsigned kNoisePeriod[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
constexpr unsigned kDmcPeriod[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
constexpr double kFdsMasterVol[4] = {1.0, 2.0 / 3.0, 0.5, 0.4};

// Licznik dlugosci: zywy albo zatrzymany ("halt" = flaga petli/kontroli).
struct Len {
    bool halted = false;
    std::uint64_t end = 0;      // absolutny tick konca (gdy nie zatrzymany)
    std::uint64_t frozen = 0;   // pozostale ticki (gdy zatrzymany)
    void load(std::uint64_t now, std::uint64_t ticks)
    {
        if (halted) frozen = ticks;
        else end = now + ticks;
    }
    void clear() { frozen = 0; end = 0; }
    void setHalt(bool h, std::uint64_t now)
    {
        if (h == halted) return;
        if (h) frozen = end > now ? end - now : 0;
        else end = now + frozen;
        halted = h;
    }
    [[nodiscard]] bool alive(std::uint64_t now) const { return halted ? frozen > 0 : now < end; }
    [[nodiscard]] std::uint64_t endTick() const { return halted ? kNever : end; }
};

class NesApu final : public ChipDecoder {
public:
    NesApu(Host& h, std::size_t first, double clock, bool fds) : ChipDecoder(h, first), clock_(clock), fds_(fds)
    {
        wave_.assign(64, 0);
    }

    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        if (reg < 0x20) apuWrite(reg, val);
        else if (fds_) fdsWrite(reg, val);
    }

    [[nodiscard]] std::uint64_t deadline() const override
    {
        std::uint64_t d = kNever;
        const std::uint64_t now = host_.now;
        auto take = [&](std::uint64_t t) { if (t > now && t < d) d = t; };
        for (int i = 0; i < 2; ++i) {
            if (!sq_[i].on) continue;
            take(sq_[i].len.endTick());
            take(envEnd(sq_[i].r0, sq_[i].trigAt));
        }
        if (tri_.on) {
            take(tri_.len.endTick());
            if (!tri_.control) take(tri_.linEnd);
        }
        if (noi_.on) {
            take(noi_.len.endTick());
            take(envEnd(noi_.r0, noi_.trigAt));
        }
        if (dmcOn_) take(dmcEnd_);
        return d;
    }
    void onDeadline(std::uint64_t /*when*/) override
    {
        for (int i = 0; i < 2; ++i) updateSquare(i, false);
        updateTriangle(false);
        updateNoise(false);
        if (dmcOn_ && dmcEnd_ != kNever && host_.now >= dmcEnd_) {
            dmcOn_ = false;
            host_.rec.noteOff(base_ + 4);
        }
    }

private:
    struct Square {
        bool enable = true;
        bool on = false;   // ostatnio zgloszony stan (do terminow)
        std::uint8_t r0 = 0, r1 = 0;
        std::uint16_t timer = 0;
        Len len;
        std::uint64_t trigAt = 0;
        Tracked tr;
    };
    struct Triangle {
        bool enable = true;
        bool on = false;
        bool control = false;
        bool triggered = false;
        std::uint8_t reload = 0;
        std::uint16_t timer = 0;
        Len len;
        std::uint64_t linEnd = 0;
        Tracked tr;
    };
    struct Noise {
        bool enable = true;
        bool on = false;
        std::uint8_t r0 = 0, r2 = 0;
        Len len;
        std::uint64_t trigAt = 0;
        Tracked tr;
    };

    [[nodiscard]] double quarter() const { return 7458.0 * 44100.0 / clock_ * (fiveStep_ ? 1.25 : 1.0); }
    [[nodiscard]] std::uint64_t lengthTicks(std::uint8_t v) const
    {
        return ticksFromSeconds(kLengthTable[(v >> 3) & 0x1F] * 2.0 * quarter() / 44100.0);
    }
    // Tick, w ktorym wygasa obwiednia sprzetowa (kNever = stala glosnosc albo petla obwiedni).
    [[nodiscard]] std::uint64_t envEnd(std::uint8_t r0, std::uint64_t trigAt) const
    {
        if ((r0 & 0x10) || (r0 & 0x20)) return kNever;
        return trigAt + static_cast<std::uint64_t>(std::ceil((1.0 + 15.0 * ((r0 & 15) + 1)) * quarter()));
    }
    [[nodiscard]] bool envAudible(std::uint8_t r0, std::uint64_t trigAt) const
    {
        if (r0 & 0x10) return (r0 & 15) > 0;
        const std::uint64_t e = envEnd(r0, trigAt);
        return e == kNever || host_.now < e;
    }
    [[nodiscard]] static std::uint8_t envVol(std::uint8_t r0)
    {
        return (r0 & 0x10) ? scaleVolume(r0 & 15, 15) : std::uint8_t{0x40};
    }

    // --- APU ($4000..$401F) ---
    void apuWrite(std::uint8_t reg, std::uint8_t val)
    {
        const std::uint64_t now = host_.now;
        switch (reg) {
        case 0x00: case 0x04: {
            const int i = reg >> 2;
            Square& s = sq_[i];
            s.r0 = val;
            s.len.setHalt((val & 0x20) != 0, now);
            updateSquare(i, false);
            break;
        }
        case 0x01: case 0x05: {
            const int i = reg >> 2;
            sq_[i].r1 = val;
            updateSquare(i, false);
            break;
        }
        case 0x02: case 0x06: {
            const int i = reg >> 2;
            sq_[i].timer = static_cast<std::uint16_t>((sq_[i].timer & 0x700) | val);
            updateSquare(i, false);
            break;
        }
        case 0x03: case 0x07: {
            const int i = reg >> 2;
            Square& s = sq_[i];
            s.timer = static_cast<std::uint16_t>((s.timer & 0x0FF) | ((val & 7) << 8));
            if (s.enable) s.len.load(now, lengthTicks(val));
            s.trigAt = now;
            updateSquare(i, true);
            break;
        }
        case 0x08:
            tri_.control = (val & 0x80) != 0;
            tri_.reload = val & 0x7F;
            tri_.len.setHalt(tri_.control, now);
            if (!tri_.control && tri_.triggered && tri_.linEnd == kNever)
                tri_.linEnd = now + static_cast<std::uint64_t>(std::ceil(tri_.reload * quarter()));
            if (tri_.control) tri_.linEnd = kNever;
            updateTriangle(false);
            break;
        case 0x0A:
            tri_.timer = static_cast<std::uint16_t>((tri_.timer & 0x700) | val);
            updateTriangle(false);
            break;
        case 0x0B:
            tri_.timer = static_cast<std::uint16_t>((tri_.timer & 0x0FF) | ((val & 7) << 8));
            if (tri_.enable) tri_.len.load(now, lengthTicks(val));
            tri_.triggered = true;
            tri_.linEnd = tri_.control ? kNever
                                       : now + static_cast<std::uint64_t>(std::ceil((1.0 + tri_.reload) * quarter()));
            updateTriangle(true);
            break;
        case 0x0C:
            noi_.r0 = val;
            noi_.len.setHalt((val & 0x20) != 0, now);
            updateNoise(false);
            break;
        case 0x0E:
            noi_.r2 = val;
            updateNoise(false);
            break;
        case 0x0F:
            if (noi_.enable) noi_.len.load(now, lengthTicks(val));
            noi_.trigAt = now;
            updateNoise(true);
            break;
        case 0x10: dmcRate_ = val & 0x0F; dmcLoop_ = (val & 0x40) != 0; break;
        case 0x12: dmcAddr_ = val; break;
        case 0x13: dmcLen_ = val; break;
        case 0x15: {
            for (int i = 0; i < 2; ++i) {
                sq_[i].enable = (val >> i) & 1;
                if (!sq_[i].enable) sq_[i].len.clear();
                updateSquare(i, false);
            }
            tri_.enable = (val & 4) != 0;
            if (!tri_.enable) tri_.len.clear();
            updateTriangle(false);
            noi_.enable = (val & 8) != 0;
            if (!noi_.enable) noi_.len.clear();
            updateNoise(false);
            if (val & 0x10) {
                if (!dmcPlaying()) dmcStart();
            } else if (dmcOn_) {
                dmcOn_ = false;
                host_.rec.noteOff(base_ + 4);
            }
            break;
        }
        case 0x17: fiveStep_ = (val & 0x80) != 0; break;
        default: break;
        }
    }

    void updateSquare(int i, bool retrigger)
    {
        Square& s = sq_[i];
        bool audible = s.enable && s.len.alive(host_.now) && s.timer >= 8 && envAudible(s.r0, s.trigAt);
        if (audible) {   // sweep: cel >= 0x800 wycisza kanal
            const unsigned shift = s.r1 & 7;
            const bool neg = (s.r1 & 8) != 0;
            int shifted = s.timer >> shift;
            if (i == 0 && neg) ++shifted;
            const int target = s.timer + (neg ? -shifted : shifted);
            if (target >= 0x800) audible = false;
        }
        const int midi = midiFromHz(clock_ / (16.0 * (s.timer + 1.0)));
        emitState(host_.rec, base_ + static_cast<std::size_t>(i), s.tr, audible, midi,
                  static_cast<std::uint8_t>(s.r0 >> 6), envVol(s.r0), retrigger && audible);
        s.on = s.tr.on;
    }

    void updateTriangle(bool retrigger)
    {
        Triangle& t = tri_;
        const std::uint64_t now = host_.now;
        const bool lin = t.control ? (t.reload > 0 && t.triggered) : now < t.linEnd;
        const bool audible = t.enable && t.len.alive(now) && lin && t.timer > 0;
        const int midi = midiFromHz(clock_ / (32.0 * (t.timer + 1.0)));
        emitState(host_.rec, base_ + 2, t.tr, audible, midi, 0, 0x40, retrigger && audible);
        t.on = t.tr.on;
    }

    void updateNoise(bool retrigger)
    {
        Noise& n = noi_;
        const bool audible = n.enable && n.len.alive(host_.now) && envAudible(n.r0, n.trigAt);
        int midi = midiFromHz(clock_ / (32.0 * kNoisePeriod[n.r2 & 15]));
        if (midi < 0) midi = 127;
        emitState(host_.rec, base_ + 3, n.tr, audible, midi, (n.r2 & 0x80) ? 2 : 1, envVol(n.r0),
                  retrigger && audible);
        n.on = n.tr.on;
    }

    // --- DPCM ---
    [[nodiscard]] bool dmcPlaying() const { return dmcOn_ && (dmcEnd_ == kNever || host_.now < dmcEnd_); }
    void dmcStart()
    {
        const unsigned addr = 0xC000u + 64u * dmcAddr_;
        const unsigned bytes = 16u * dmcLen_ + 1u;
        const unsigned period = kDmcPeriod[dmcRate_];
        char key[16];
        std::snprintf(key, sizeof key, "%04X:%03X", addr, bytes);
        const std::uint8_t inst = dmcBank_.id(key);
        dmcEnd_ = dmcLoop_ ? kNever : host_.now + ticksFromSeconds(bytes * 8.0 * period / clock_);
        dmcOn_ = true;
        const int midi = midiFromRatio(static_cast<double>(kDmcPeriod[15]) / period, 1.0);
        host_.rec.noteOn(base_ + 4, midi < 0 ? 60 : midi, inst, 0x40);
    }

    // --- FDS ($4080..) ---
    void fdsWrite(std::uint8_t reg, std::uint8_t val)
    {
        if (reg == 0x23) { fdsIo_ = (val & 2) != 0; updateFds(); return; }
        if (!fdsIo_) return;
        if (reg >= 0x40 && reg < 0x80) {
            if (fdsWavWrite_) wave_[reg - 0x40] = val & 0x3F;
            return;
        }
        switch (reg) {
        case 0x80: fdsVolReg_ = val; updateFds(); break;
        case 0x82: fdsFreq_ = static_cast<std::uint16_t>((fdsFreq_ & 0xF00) | val); updateFds(); break;
        case 0x83:
            fdsFreq_ = static_cast<std::uint16_t>((fdsFreq_ & 0x0FF) | ((val & 0x0F) << 8));
            fdsHalt_ = (val & 0x80) != 0;
            updateFds();
            break;
        case 0x89: fdsWavWrite_ = (val & 0x80) != 0; fdsMaster_ = val & 3; updateFds(); break;
        default: break;
        }
    }
    void updateFds()
    {
        const bool envOff = (fdsVolReg_ & 0x80) != 0;
        const int lvl = std::min(fdsVolReg_ & 0x3F, 32);
        const bool audible = fdsIo_ && !fdsHalt_ && fdsFreq_ > 0 && (!envOff || lvl > 0);
        const int midi = midiFromHz(fdsFreq_ * clock_ / 4194304.0);
        const double base = envOff ? lvl / 32.0 : 1.0;
        const auto vol = static_cast<std::uint8_t>(std::lround(64.0 * base * kFdsMasterVol[fdsMaster_ & 3]));
        std::string key(wave_.begin(), wave_.end());
        emitState(host_.rec, base_ + 5, fdsTr_, audible, midi, audible ? fdsBank_.id(key) : kValueNone, vol);
    }

    double clock_;
    bool fds_;
    bool fiveStep_ = false;
    Square sq_[2];
    Triangle tri_;
    Noise noi_;
    // DPCM
    std::uint8_t dmcRate_ = 0, dmcAddr_ = 0, dmcLen_ = 0;
    bool dmcLoop_ = false, dmcOn_ = false;
    std::uint64_t dmcEnd_ = kNever;
    InstrumentBank dmcBank_;
    // FDS
    bool fdsIo_ = false, fdsHalt_ = true, fdsWavWrite_ = false;
    std::uint8_t fdsVolReg_ = 0, fdsMaster_ = 0;
    std::uint16_t fdsFreq_ = 0;
    std::vector<std::uint8_t> wave_;
    InstrumentBank fdsBank_;
    Tracked fdsTr_;
};

} // namespace

std::unique_ptr<ChipDecoder> makeNesApu(Host& h, std::size_t first, std::uint32_t clockHz, std::uint32_t flags)
{
    return std::make_unique<NesApu>(h, first, static_cast<double>(clockHz), (flags & 0x80000000u) != 0);
}

} // namespace vgmplay::chipstate::detail
