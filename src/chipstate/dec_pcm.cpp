// vgmplay - modul chipstate: uklady PCM/ADPCM - C140, K054539, K053260, Sega PCM, Irem GA20, OKIM6258.
//
// Wspolne zasady (jak OKIM6295):
//  * key-on = nuta; instrument = numer unikatowej probki (adres startu + parametry) w kolejnosci pojawienia sie
//    (0..254, powyzej 254); glosnosc z rejestru glosnosci liniowo -> 0x00..0x40;
//  * wysokosc: nuta = 60 + 12 * log2(ratio), gdzie ratio = liczba probek odczytywanych na JEDNA probke wyjsciowa
//    ukladu (tempo natywne ukladu = 1.0 = C-4, 0.5 = C-3, 2.0 = C-5). Czestotliwosc probki w pliku nie jest znana,
//    wiec to nuta WZGLEDNA: ta sama probka z tym samym rejestrem tempa = ta sama nuta; obciete do 0..127;
//  * key-off: jawny (rejestr key-off / zerowy bit) albo na koncu probki, gdy da sie go wyznaczyc z rejestrow
//    (C140, K053260, Sega PCM: dlugosc/adres koncowy; K054539, GA20: znacznik konca w ROM z bloku 0x67);
//    petla = nuta trwa do key-off.
// Zmiany tempa w trakcie trwania probki nie generuja nowych nut (tylko key-on); zmiana glosnosci - zdarzenie volume.
#include <array>
#include <cstdio>
#include <unordered_map>

#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

// Baza: glosy z terminem konca probki.
class PcmBase : public ChipDecoder {
public:
    [[nodiscard]] std::uint64_t deadline() const override
    {
        std::uint64_t d = kNever;
        for (const Voice& v : v_)
            if (v.playing) d = std::min(d, v.end);
        return d;
    }
    void onDeadline(std::uint64_t when) override
    {
        for (std::size_t i = 0; i < v_.size(); ++i)
            if (v_[i].playing && v_[i].end <= when) keyOff(i);
    }

protected:
    struct Voice {
        bool playing = false;
        std::uint64_t end = kNever;
    };
    PcmBase(Host& h, std::size_t first, std::size_t voices) : ChipDecoder(h, first), v_(voices) {}

    void keyOn(std::size_t ch, int midi, std::uint8_t inst, std::uint8_t vol, std::uint64_t end)
    {
        v_[ch].playing = true;
        v_[ch].end = end;
        host_.rec.noteOn(base_ + ch, midi < 0 ? 60 : midi, inst, vol);
    }
    virtual void keyOff(std::size_t ch)
    {
        if (v_[ch].playing) host_.rec.noteOff(base_ + ch);
        v_[ch] = Voice{};
    }
    void setVolume(std::size_t ch, std::uint8_t vol)
    {
        if (v_[ch].playing) host_.rec.volume(base_ + ch, vol);
    }
    [[nodiscard]] std::uint64_t endAfter(double seconds) const { return host_.now + ticksFromSeconds(seconds); }
    std::uint8_t sampleId(const char* fmt, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0)
    {
        char key[64];
        std::snprintf(key, sizeof key, fmt, a, b, c);
        return bank_.id(key);
    }

    std::vector<Voice> v_;
    InstrumentBank bank_;
};

// --- C140 (24 glosy; rejestry 0x00..0x17F, po 16 bajtow na glos) ----------------------------------
// Key-on bez czestotliwosci (zero) = glos milczy; nuta powstaje przy pierwszym niezerowym zapisie czestotliwosci.
// Offsety w glosie: 0 vol prawy, 1 vol lewy, 2..3 czestotliwosc (16 bit), 4 bank, 5 tryb (bit 7 key-on, 4 petla),
// 6..7 start, 8..9 koniec, 10..11 petla. Tempo: czestotliwosc / 65536 probek na probke wyjsciowa (zegar / 288).
class C140 final : public PcmBase {
public:
    C140(Host& h, std::size_t first, std::uint32_t clock) : PcmBase(h, first, 24)
    {
        std::uint32_t eff = clock;
        if (clock == 21390u) eff = 12288000u;
        else if (clock < 1000000u) eff = clock * 576u;
        rate_ = eff / 288.0;
        regs_.fill(0);
    }
    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override { writeMem(reg, val); }
    void writeMem(std::uint32_t addr, std::uint8_t val) override
    {
        addr &= 0x1FF;
        regs_[addr] = val;
        if (addr >= 0x180) return;
        const std::size_t ch = addr >> 4;
        const unsigned r = addr & 15;
        if (r == 5) {
            if (val & 0x80) {
                keyed_[ch] = true;
                on(ch);
            } else {
                keyOff(ch);
            }
        } else if (r <= 1) {
            setVolume(ch, vol(ch));
        } else if ((r == 2 || r == 3) && keyed_[ch] && !v_[ch].playing) {
            on(ch);   // sterowniki ustawiaja czestotliwosc PO key-on (do tego czasu glos milczy)
        }
    }

protected:
    void keyOff(std::size_t ch) override
    {
        keyed_[ch] = false;
        PcmBase::keyOff(ch);
    }

private:
    [[nodiscard]] std::uint8_t vol(std::size_t ch) const
    {
        return scaleVolume(std::max(regs_[ch * 16], regs_[ch * 16 + 1]), 255);
    }
    void on(std::size_t ch)
    {
        const std::uint8_t* r = &regs_[ch * 16];
        const unsigned freq = (r[2] << 8) | r[3];
        const unsigned start = (r[6] << 8) | r[7], end = (r[8] << 8) | r[9];
        if (freq == 0) return;   // rdzen: glos bez czestotliwosci milczy; nuta powstanie po zapisie czestotliwosci
        if (end <= start) { keyOff(ch); return; }   // pusta probka = cisza
        const std::uint64_t until =
            (r[5] & 0x10) ? kNever : endAfter((end - start) * 65536.0 / freq / rate_);
        keyOn(ch, midiFromRatio(freq / 65536.0, 1.0), sampleId("%03X:%04X:%04X", r[4], start, end), vol(ch), until);
    }

    double rate_;
    std::array<std::uint8_t, 0x200> regs_;
    std::array<bool, 24> keyed_{};
};

// --- K054539 (8 glosow; 0x20 bajtow na glos, typ/petla w 0x200+2*ch, key-on 0x214, key-off 0x215) ---------
// Tempo: wartosc 24-bit (rejestry 0..2) / 65536 probek na probke wyjsciowa (zegar / 384). Koniec probki
// (bez petli): znacznik w ROM - 0x80 (PCM 8 bit), 0x8000 (PCM 16 bit), 0x88 (DPCM 4 bit); wymaga bloku ROM 0x8C.
class K054539 final : public PcmBase {
public:
    K054539(Host& h, std::size_t first, std::uint32_t clock, std::uint32_t flags)
        : PcmBase(h, first, 8), latchMode_((flags & 4u) != 0)
    {
        const std::uint32_t eff = clock < 1000000u ? clock * 384u : clock;
        rate_ = eff / 384.0;
        regs_.fill(0);
        latch_.fill(0);
    }
    void write(std::uint8_t /*port*/, std::uint8_t /*reg*/, std::uint8_t /*val*/) override {}
    void romBlock(std::uint8_t type, std::uint32_t memSize, std::uint32_t startAddr,
                  std::span<const std::uint8_t> data) override
    {
        if (type != 0x8C) return;
        if (!rom_.present()) rom_.setSize(memSize);
        rom_.add(startAddr, data);
        endMemo_.clear();
    }
    void writeMem(std::uint32_t addr, std::uint8_t val) override
    {
        if (addr >= regs_.size()) return;
        const bool latch = latchMode_ && (regs_[0x22F] & 1);
        if (latch && addr < 0x100) {
            const int offs = static_cast<int>(addr & 0x1F) - 0xC;
            if (offs >= 0 && offs <= 2) { latch_[(addr >> 5) * 3 + offs] = val; return; }
        }
        regs_[addr] = val;
        if (addr == 0x214) {
            for (std::size_t ch = 0; ch < 8; ++ch) {
                if (!((val >> ch) & 1)) continue;
                if (latch)
                    for (int k = 0; k < 3; ++k) regs_[ch * 32 + 0xC + k] = latch_[ch * 3 + k];
                on(ch);
            }
        } else if (addr == 0x215) {
            for (std::size_t ch = 0; ch < 8; ++ch)
                if ((val >> ch) & 1) keyOff(ch);
        } else if (addr < 0x100 && (addr & 0x1F) == 3) {
            setVolume(addr >> 5, vol(addr >> 5));
        }
    }

private:
    [[nodiscard]] std::uint8_t vol(std::size_t ch) const
    {
        return static_cast<std::uint8_t>(std::lround(64.0 * std::pow(10.0, -36.0 * regs_[ch * 32 + 3] / 64.0 / 20.0)));
    }
    // Liczba krokow do znacznika konca (0 = nieznana).
    std::uint32_t stepsToEnd(std::uint32_t start, unsigned type, bool rev)
    {
        if (!rom_.present()) return 0;
        const std::uint64_t key = (static_cast<std::uint64_t>(start) << 8) | (type << 1) | (rev ? 1u : 0u);
        auto it = endMemo_.find(key);
        if (it != endMemo_.end()) return it->second;
        std::uint32_t n = 0;
        const int dir = rev ? -1 : 1;
        constexpr std::uint32_t kMax = 4u << 20;
        for (std::uint32_t k = 1; k <= kMax && n == 0; ++k) {
            if (type == 0) {
                if (rom_.at(static_cast<std::uint32_t>(start + dir * static_cast<std::int64_t>(k))) == 0x80) n = k;
            } else if (type == 4) {
                const auto a = static_cast<std::uint32_t>(start + dir * 2 * static_cast<std::int64_t>(k));
                if (rom_.at(a) == 0x00 && rom_.at(a + 1) == 0x80) n = k;
            } else {   // DPCM: pozycja w nibblach
                const std::int64_t nib = 2 * static_cast<std::int64_t>(start) + dir * static_cast<std::int64_t>(k);
                if (rom_.at(static_cast<std::uint32_t>(nib >> 1)) == 0x88) n = k;
            }
        }
        endMemo_[key] = n;
        return n;
    }
    void on(std::size_t ch)
    {
        if (!(regs_[0x22F] & 1)) return;   // PCM wylaczony: uklad milczy
        const std::uint8_t* r = &regs_[ch * 32];
        const std::uint32_t delta = r[0] | (r[1] << 8) | (r[2] << 16);
        const std::uint32_t start = r[0xC] | (r[0xD] << 8) | (r[0xE] << 16);
        const unsigned type = regs_[0x200 + 2 * ch] & 0xC;
        const bool rev = (regs_[0x200 + 2 * ch] & 0x20) != 0;
        const bool loop = (regs_[0x201 + 2 * ch] & 1) != 0;
        std::uint64_t until = kNever;
        if (!loop && delta > 0) {
            const std::uint32_t n = stepsToEnd(start, type, rev);
            if (n > 0) until = endAfter(n * 65536.0 / delta / rate_);
        }
        keyOn(ch, midiFromRatio(delta / 65536.0, 1.0), sampleId("%06X:%02X:%X", start, type, rev ? 1u : 0u), vol(ch),
              until);
    }

    bool latchMode_;
    double rate_;
    std::array<std::uint8_t, 0x230> regs_;
    std::array<std::uint8_t, 24> latch_;
    RomMap rom_;
    std::unordered_map<std::uint64_t, std::uint32_t> endMemo_;
};

// --- K053260 (4 glosy; rejestry 0x08..0x27 po 8 bajtow, key-on/off 0x28) ----------------------------
// Krok odczytu co (4096 - pitch) taktow zegara, uklad wyprowadza probke co 64 takty => ratio = 64 / (4096 - pitch).
// Dlugosc w bajtach (ADPCM: 2 kroki na bajt). Wymaga bitu 1 rejestru 0x2F (wyjscie wlaczone).
class K053260 final : public PcmBase {
public:
    K053260(Host& h, std::size_t first, std::uint32_t clock) : PcmBase(h, first, 4), clock_(clock) { regs_.fill(0); }
    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        reg &= 0x3F;
        regs_[reg] = val;
        if (reg >= 0x08 && reg <= 0x27) {
            if ((reg & 7) == 7) setVolume((reg - 8u) / 8u, scaleVolume(val & 0x7F, 0x7F));
        } else if (reg == 0x28) {
            const std::uint8_t rising = val & ~keyon_;
            for (std::size_t i = 0; i < 4; ++i) {
                if (rising & (1u << i)) on(i);
                else if (!(val & (1u << i))) keyOff(i);
            }
            keyon_ = val;
        }
    }

private:
    void on(std::size_t v)
    {
        if (!(regs_[0x2F] & 2)) return;
        const std::uint8_t* r = &regs_[8 + 8 * v];
        const unsigned pitch = r[0] | ((r[1] & 0x0F) << 8);
        const unsigned length = r[2] | (r[3] << 8);
        const unsigned start = r[4] | (r[5] << 8) | ((r[6] & 0x1F) << 16);
        const bool kadpcm = (regs_[0x2A] >> (v + 4)) & 1;
        const bool loop = (regs_[0x2A] >> v) & 1;
        if (length == 0) { keyOff(v); return; }
        const double stepHz = clock_ / (4096.0 - pitch);
        const std::uint64_t until = loop ? kNever : endAfter((kadpcm ? 2.0 : 1.0) * length / stepHz);
        keyOn(v, midiFromRatio(64.0 / (4096.0 - pitch), 1.0), sampleId("%06X:%04X:%X", start, length, kadpcm ? 1u : 0u),
              scaleVolume(r[7] & 0x7F, 0x7F), until);
    }

    double clock_;
    std::uint8_t keyon_ = 0;
    std::array<std::uint8_t, 0x40> regs_;
};

// --- Sega PCM (16 glosow; 8 bajtow na glos: +2/+3 glosnosc, +6 strona konca, +7 delta; +0x84/+0x85 adres biezacy,
// +0x86 bit 0 = glos wylaczony, bit 1 = petla wylaczona, reszta = bank) --------------------------------------
// Tempo: delta / 256 probek na probke wyjsciowa (zegar / 128). Koniec: adres (strona koncowa + 1) << 16, tylko gdy
// petla wylaczona. Key-on = zapis +0x86 z bitem 0 = 0 (gdy glos nie gra); zapis adresu (+0x84/+0x85) w trakcie gry
// = skok do nowej probki (nowa nuta).
class SegaPcm final : public PcmBase {
public:
    SegaPcm(Host& h, std::size_t first, std::uint32_t clock) : PcmBase(h, first, 16), rate_(clock / 128.0)
    {
        ram_.fill(0xFF);
    }
    void write(std::uint8_t /*port*/, std::uint8_t /*reg*/, std::uint8_t /*val*/) override {}
    void writeMem(std::uint32_t addr, std::uint8_t val) override
    {
        const std::uint32_t off = addr & 0x7FF;
        ram_[off] = val;
        if (off >= 0x100) return;
        const std::size_t ch = (off >> 3) & 15;
        const unsigned r = (off & 7) | (off & 0x80);
        switch (r) {
        case 2: case 3: setVolume(ch, vol(ch)); break;
        case 0x84: case 0x85:
            if (v_[ch].playing && !(ram_[ch * 8 + 0x86] & 1)) on(ch);
            break;
        case 0x86:
            if (val & 1) keyOff(ch);
            else if (!v_[ch].playing) on(ch);
            break;
        default: break;
        }
    }

private:
    [[nodiscard]] std::uint8_t vol(std::size_t ch) const
    {
        return scaleVolume(std::max(ram_[ch * 8 + 2] & 0x7F, ram_[ch * 8 + 3] & 0x7F), 0x7F);
    }
    void on(std::size_t ch)
    {
        const std::uint8_t* r = &ram_[ch * 8];
        const std::uint32_t addr = (r[0x85] << 16) | (r[0x84] << 8);
        const unsigned delta = r[7];
        std::uint64_t until = kNever;
        if ((r[0x86] & 2) && delta > 0) {
            std::uint32_t endAddr = (static_cast<std::uint32_t>(r[6]) + 1u) << 16;
            if (addr >= endAddr) endAddr += 0x1000000u;
            until = endAfter((endAddr - addr) / static_cast<double>(delta) / rate_);
        }
        keyOn(ch, midiFromRatio(delta / 256.0, 1.0), sampleId("%02X:%06X", r[0x86] & 0xFC, addr), vol(ch), until);
    }

    double rate_;
    std::array<std::uint8_t, 0x800> ram_;
};

// --- Irem GA20 (4 glosy; 8 bajtow: 0..1 start, 2..3 koniec, 4 tempo, 5 glosnosc, 6 sterowanie bit 1 = key-on) ---
// Krok probki co (256 - tempo) probek wyjsciowych (zegar / 4) => ratio = 1 / (256 - tempo). Koniec: bajt 0 w ROM
// (blok 0x93), gdy ROM dostepny. Glosnosc: v * 256 / (v + 10) jak w rdzeniu.
class Ga20 final : public PcmBase {
public:
    Ga20(Host& h, std::size_t first, std::uint32_t clock) : PcmBase(h, first, 4), rate_(clock / 4.0) { regs_.fill(0); }
    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        reg &= 0x1F;
        regs_[reg] = val;
        const std::size_t ch = reg >> 3;
        const unsigned r = reg & 7;
        if (r == 5) setVolume(ch, vol(ch));
        else if (r == 6) {
            if (val & 2) on(ch);
            else keyOff(ch);
        }
    }
    void romBlock(std::uint8_t type, std::uint32_t memSize, std::uint32_t startAddr,
                  std::span<const std::uint8_t> data) override
    {
        if (type != 0x93) return;
        if (!rom_.present()) rom_.setSize(memSize);
        rom_.add(startAddr, data);
    }

private:
    [[nodiscard]] std::uint8_t vol(std::size_t ch) const
    {
        const int v = regs_[ch * 8 + 5];
        return scaleVolume(v * 256 / (v + 10), 255 * 256 / 265);
    }
    void on(std::size_t ch)
    {
        const std::uint8_t* r = &regs_[ch * 8];
        const std::uint32_t start = (r[0] << 4) | (r[1] << 12);
        const unsigned rate = r[4];
        std::uint64_t until = kNever;
        if (rom_.present()) {
            std::uint32_t n = 0;
            while (n < (1u << 20) && rom_.at(start + n) != 0) ++n;
            if (n == 0) { keyOff(ch); return; }   // znacznik konca na starcie: cisza
            if (n < (1u << 20)) until = endAfter(static_cast<double>(n) * (256 - rate) / rate_);
        }
        keyOn(ch, midiFromRatio(1.0 / (256.0 - rate), 1.0), sampleId("%05X:%02X", start, 0), vol(ch), until);
    }

    double rate_;
    std::array<std::uint8_t, 0x20> regs_;
    RomMap rom_;
};

// --- OKIM6258 (1 kanal; 0 = sterowanie: bit 0 stop, bit 1 start; 0x0C = dzielnik) ---------------------------
// Dane ADPCM zwykle plyna strumieniem DAC (0x90..0x95): start strumienia = instrument (numer bloku / adresu
// startu) i termin konca. Key-on = start (bit 1) gdy kanal nie gral; start strumienia podczas gry = nowa nuta.
// Nuta z czestotliwosci probkowania: clock / dzielnik (1024, 768, 512, 512): 512 = C-4, 768 = F-3 (+5), 1024 = C-3.
class Okim6258 final : public ChipDecoder {
public:
    Okim6258(Host& h, std::size_t first, std::uint32_t flags) : ChipDecoder(h, first), div_(flags & 3u) {}
    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        if (reg == 0x00) {
            if (val & 1) stop();
            else if (val & 2) {
                if (!playing_) {
                    playing_ = true;
                    noteOn(streamTick_ == host_.now ? inst_ : kValueNone);
                }
            } else {
                stop();
            }
        } else if (reg == 0x0C) {
            div_ = val & 3u;
        }
    }
    void dacStreamStart(std::uint8_t instrument, std::uint64_t endTick) override
    {
        inst_ = instrument;
        end_ = endTick;
        streamTick_ = host_.now;
        if (playing_) noteOn(instrument);
    }
    void dacStreamStop() override
    {
        end_ = kNever;
        host_.rec.noteOff(base_);
    }
    [[nodiscard]] std::uint64_t deadline() const override { return playing_ ? end_ : kNever; }
    void onDeadline(std::uint64_t when) override
    {
        if (playing_ && end_ <= when) {
            end_ = kNever;
            host_.rec.noteOff(base_);
        }
    }

private:
    void stop()
    {
        if (playing_) host_.rec.noteOff(base_);
        playing_ = false;
        end_ = kNever;
    }
    void noteOn(std::uint8_t inst)
    {
        static constexpr double kDiv[4] = {1024.0, 768.0, 512.0, 512.0};
        host_.rec.noteOn(base_, midiFromRatio(512.0 / kDiv[div_ & 3], 1.0), inst, 0x40);
    }

    unsigned div_;
    bool playing_ = false;
    std::uint8_t inst_ = kValueNone;
    std::uint64_t end_ = kNever, streamTick_ = kNever;
};

} // namespace

std::unique_ptr<ChipDecoder> makeC140(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<C140>(h, first, clockHz);
}
std::unique_ptr<ChipDecoder> makeK054539(Host& h, std::size_t first, std::uint32_t clockHz, std::uint32_t flags)
{
    return std::make_unique<K054539>(h, first, clockHz, flags);
}
std::unique_ptr<ChipDecoder> makeK053260(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<K053260>(h, first, clockHz);
}
std::unique_ptr<ChipDecoder> makeSegaPcm(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<SegaPcm>(h, first, clockHz);
}
std::unique_ptr<ChipDecoder> makeGa20(Host& h, std::size_t first, std::uint32_t clockHz)
{
    return std::make_unique<Ga20>(h, first, clockHz);
}
std::unique_ptr<ChipDecoder> makeOkim6258(Host& h, std::size_t first, std::uint32_t /*clockHz*/, std::uint32_t flags)
{
    return std::make_unique<Okim6258>(h, first, flags);
}

} // namespace vgmplay::chipstate::detail
