// vgmplay - modul chipstate: OKI MSM6295 (4 glosy ADPCM).
//
//  * start probki (komenda 2-bajtowa: numer frazy + maska glosow/tlumienie) = nuta C-4 (60) na kanale glosu,
//    instrument = numer frazy (0..127), glosnosc z tlumienia (tablica ukladu: 0 dB..-24 dB; >= 9 = cisza -> 0);
//  * stop (komenda z bitami 3..6) = key-off;
//  * koniec probki: jesli w strumieniu byl blok ROM (0x67 typ 0x8B), z tabeli fraz na poczatku ROM (8 B na
//    fraze: start[3] stop[3], big-endian, maska 0x3FFFF) liczona jest dlugosc (2 * (stop - start + 1) nibbli,
//    jeden nibbl na probke wyjsciowa, czestotliwosc = zegar / (pin7 ? 132 : 165)) i na jej koncu zapisywany jest
//    key-off. Bez ROM (albo fraza poza pierwszym 1 KiB) nuta trwa do stopu / nastepnego startu.
//    Fraza o start >= stop jest ignorowana przez uklad - tak samo tu (brak nuty).
//  * Bankowanie (NMK112, bank_offs) NIE jest sledzone: tabela fraz czytana z banku 0.
#include <array>

#include "cs_internal.hpp"

namespace vgmplay::chipstate::detail {
namespace {

constexpr std::size_t kRomHead = 1024;   // 128 fraz x 8 B
constexpr int kVolumeTable[16] = {0x20, 0x16, 0x10, 0x0B, 0x08, 0x06, 0x04, 0x03, 0x02, 0, 0, 0, 0, 0, 0, 0};

class Okim6295 final : public ChipDecoder {
public:
    Okim6295(Host& h, std::size_t first, std::uint32_t clock, bool pin7)
        : ChipDecoder(h, first), clock_(clock), pin7_(pin7)
    {
        rom_.fill(0);
    }

    void write(std::uint8_t /*port*/, std::uint8_t reg, std::uint8_t val) override
    {
        if (reg == 0x00) command(val);
        else if (reg == 0x0C) pin7_ = val != 0;
    }

    void romBlock(std::uint8_t type, std::uint32_t /*memSize*/, std::uint32_t startAddr,
                  std::span<const std::uint8_t> data) override
    {
        if (type != 0x8B) return;
        romSeen_ = true;
        if (startAddr >= kRomHead) return;
        const std::size_t n = std::min<std::size_t>(data.size(), kRomHead - startAddr);
        std::copy_n(data.begin(), n, rom_.begin() + startAddr);
    }

    [[nodiscard]] std::uint64_t deadline() const override
    {
        std::uint64_t d = kNever;
        for (const Voice& v : voice_)
            if (v.playing) d = std::min(d, v.end);
        return d;
    }
    void onDeadline(std::uint64_t when) override
    {
        for (std::size_t i = 0; i < voice_.size(); ++i) {
            if (voice_[i].playing && voice_[i].end <= when) stop(i);
        }
    }

private:
    struct Voice {
        bool playing = false;
        std::uint64_t end = kNever;
    };

    void command(std::uint8_t val)
    {
        if (pending_ >= 0) {
            const unsigned mask = val >> 4;
            for (std::size_t v = 0; v < 4; ++v)
                if (mask & (1u << v)) start(v, static_cast<unsigned>(pending_), val & 0x0F);
            pending_ = -1;
        } else if (val & 0x80) {
            pending_ = val & 0x7F;
        } else {
            const unsigned mask = val >> 3;
            for (std::size_t v = 0; v < 4; ++v)
                if (mask & (1u << v)) stop(v);
        }
    }

    void start(std::size_t v, unsigned phrase, unsigned att)
    {
        Voice& vo = voice_[v];
        if (vo.playing && vo.end > host_.now) {
            if (vo.end != kNever) return;   // uklad ignoruje start glosu, ktory jeszcze gra
        }
        std::uint64_t end = kNever;
        if (romSeen_) {
            const std::uint8_t* p = rom_.data() + phrase * 8u;
            const std::uint32_t s = ((p[0] << 16) | (p[1] << 8) | p[2]) & 0x3FFFFu;
            const std::uint32_t e = ((p[3] << 16) | (p[4] << 8) | p[5]) & 0x3FFFFu;
            if (s >= e) {   // nieprawidlowa fraza: uklad nie gra
                if (vo.playing) stop(v);
                return;
            }
            const double rate = static_cast<double>(clock_) / (pin7_ ? 132.0 : 165.0);
            const double nibbles = 2.0 * (static_cast<double>(e) - s + 1.0);
            end = host_.now + static_cast<std::uint64_t>(nibbles * 44100.0 / rate);
        }
        vo.playing = true;
        vo.end = end;
        host_.rec.noteOn(base_ + v, 60, static_cast<std::uint8_t>(phrase), scaleVolume(kVolumeTable[att & 15], 0x20));
    }
    void stop(std::size_t v)
    {
        if (voice_[v].playing) host_.rec.noteOff(base_ + v);
        voice_[v] = Voice{};
    }

    std::uint32_t clock_;
    bool pin7_;
    bool romSeen_ = false;
    int pending_ = -1;
    std::array<std::uint8_t, kRomHead> rom_{};
    std::array<Voice, 4> voice_{};
};

} // namespace

std::unique_ptr<ChipDecoder> makeOkim6295(Host& h, std::size_t first, std::uint32_t clockHz, std::uint32_t flags)
{
    return std::make_unique<Okim6295>(h, first, clockHz, (flags & 0x80000000u) != 0);
}

} // namespace vgmplay::chipstate::detail
