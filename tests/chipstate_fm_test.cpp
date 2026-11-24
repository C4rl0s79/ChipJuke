// Testy dekoderow FM rodziny OPN/OPM (chipstate): CH3 special/CSM, retrigger (nowe zbocze key-on operatora),
// legato (zmiana tonu przy trzymanej nucie), key-on z maska 0. Syntetyczne strumienie VGM 1.71, wiersz 735 tickow.
#include <doctest/doctest.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include "vgmplay/chipstate.hpp"
#include "vgmplay/format.hpp"

namespace {

using namespace vgmplay;
using chipstate::Pattern;

constexpr std::size_t kOpnClockOfs = 0x44;   // YM2203 (offset zegara w naglowku)
constexpr std::size_t kOpmClockOfs = 0x30;   // YM2151

class Fm {
public:
    Fm(std::size_t clockOfs, std::uint32_t clock) : hdr_(0x100, 0)
    {
        std::memcpy(hdr_.data(), "Vgm ", 4);
        put32(8, 0x171);
        put32(0x34, 0x100 - 0x34);
        put32(clockOfs, clock);
    }
    Fm& raw(std::initializer_list<int> b) { for (int v : b) d_.push_back(static_cast<std::uint8_t>(v)); return *this; }
    Fm& wait(std::uint32_t n)
    {
        total_ += n;
        while (n > 0) {
            const std::uint32_t c = n > 65535 ? 65535 : n;
            raw({0x61, static_cast<int>(c & 0xFF), static_cast<int>(c >> 8)});
            n -= c;
        }
        return *this;
    }
    Fm& opn(int r, int v) { return raw({0x55, r, v}); }
    Fm& opm(int r, int v) { return raw({0x54, r, v}); }
    [[nodiscard]] std::vector<std::uint8_t> bytes() const
    {
        std::vector<std::uint8_t> b = hdr_;
        b.insert(b.end(), d_.begin(), d_.end());
        b.push_back(0x66);
        const auto eof = static_cast<std::uint32_t>(b.size() - 4);
        for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<std::uint8_t>(eof >> (8 * i));
        for (int i = 0; i < 4; ++i) b[0x18 + i] = static_cast<std::uint8_t>(total_ >> (8 * i));
        return b;
    }

private:
    void put32(std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) hdr_[o + i] = static_cast<std::uint8_t>(v >> (8 * i)); }
    std::vector<std::uint8_t> hdr_, d_;
    std::uint32_t total_ = 0;
};

Pattern build(const std::vector<std::uint8_t>& bytes)
{
    auto h = format::parseVgmHeader(bytes);
    REQUIRE_MESSAGE(h.ok(), (h.ok() ? "" : h.error().message));
    auto s = format::validateCommandStream(bytes, h.value());
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    chipstate::PatternOptions o;
    o.rowTicks = 735;
    auto p = chipstate::buildPattern(bytes, h.value(), s.value(), o);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
    return std::move(p).value();
}

// YM2203 4 MHz: sr = 4e6 / 72; fnum 1038 blok 4 => 440 Hz (A4, 69); fnum 1038 blok 5 => A5 (81).
constexpr std::uint32_t kOpnClock = 4'000'000;
void opnFreq(Fm& v, int regLatch, int regLow, int block, int fnum)
{
    v.opn(regLatch, (block << 3) | (fnum >> 8)).opn(regLow, fnum & 0xFF);
}

} // namespace

TEST_CASE("chipstate OPN: CH3 special - key-on daje nute takze gdy ton jest tylko w F-number operatorow (0xA8-0xAE)")
{
    Fm v(kOpnClockOfs, kOpnClock);
    v.opn(0xB2, 0x07);                       // ALG 7 (wszystkie nosne), TL = 0
    v.opn(0x27, 0x40);                       // CH3 special
    opnFreq(v, 0xAD, 0xA9, 4, 1038);         // operator S1: A4
    opnFreq(v, 0xAE, 0xAA, 5, 1038);         // operator S2: A5
    opnFreq(v, 0xAC, 0xA8, 3, 1038);         // operator S3
    v.opn(0x28, 0xF2);                       // key-on wszystkich operatorow kanalu 3
    v.wait(735);
    v.opn(0x28, 0x02);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 6);
    CHECK(p.at(0, 2).note == 57);            // F-number operatora z najnizszego rejestru (0xA8, blok 3 = A3)
    CHECK(p.at(1, 2).note == chipstate::kNoteOff);
    // Kanaly 1-2 nietkniete zapisami 0xA8-0xAE (to nie F-number kanalow 1-3).
    for (std::uint64_t r = 0; r < p.rowCount; ++r) {
        CHECK(p.at(r, 0).note < 0);
        CHECK(p.at(r, 1).note < 0);
    }
}

TEST_CASE("chipstate OPN: CH3 special - F-number z 0xA2/0xA6 (S4) ma pierwszenstwo, tryb normalny ignoruje 0xA8-0xAE")
{
    Fm v(kOpnClockOfs, kOpnClock);
    v.opn(0xB2, 0x07);
    v.opn(0x27, 0x40);
    opnFreq(v, 0xA6, 0xA2, 5, 1038);         // S4 = A5 (81)
    opnFreq(v, 0xAD, 0xA9, 4, 1038);         // S1 = A4 - nie ma wplywu na nute
    v.opn(0x28, 0xF2);
    v.wait(735);
    v.opn(0x28, 0x02);
    v.opn(0x27, 0x00);                       // tryb normalny: A8-AE zapisane, nie wplywaja na kanal 3
    opnFreq(v, 0xA6, 0xA2, 4, 1038);
    opnFreq(v, 0xAE, 0xAA, 6, 1038);
    v.opn(0x28, 0xF2);
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, 2).note == 81);
    CHECK(p.at(1, 2).note == 69);
}

TEST_CASE("chipstate OPN: legato (zmiana F-number przy trzymanej nucie), retrigger (nowe bity maski), powtorzona maska")
{
    Fm v(kOpnClockOfs, kOpnClock);
    v.opn(0xB0, 0x07);
    opnFreq(v, 0xA4, 0xA0, 4, 1038);
    v.opn(0x28, 0x10);                       // tylko operator 1
    v.wait(735);                             // wiersz 0: nuta A4
    v.opn(0x28, 0x10);                       // ta sama maska: nie jest nowym zboczem
    v.wait(735);                             // wiersz 1: pusty
    v.opn(0x28, 0xF0);                       // doszly operatory 2-4: ponowny atak
    v.wait(735);                             // wiersz 2: ta sama nuta jeszcze raz
    opnFreq(v, 0xA4, 0xA0, 5, 1038);         // legato: zmiana tonu bez key-on
    v.wait(735);                             // wiersz 3: A5
    v.opn(0x28, 0x00);
    v.wait(735);                             // wiersz 4: ===
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(1, 0).note == chipstate::kNoteNone);
    CHECK(p.at(2, 0).note == 69);
    CHECK(p.at(2, 0).instrument == p.at(0, 0).instrument);
    CHECK(p.at(3, 0).note == 81);
    CHECK(p.at(4, 0).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate OPM: legato, retrigger, key-on z maska 0 = key-off, key-off+key-on w jednym wierszu")
{
    Fm v(kOpmClockOfs, 3579545);
    v.opm(0x20, 0xC0 | 7);                   // ALG 7
    v.opm(0x28, 0x4A).opm(0x30, 0x00);       // A4
    v.opm(0x08, 0x08);                       // tylko operator 1 (maska 1)
    v.wait(735);                             // wiersz 0: 69
    v.opm(0x08, 0x08);                       // ta sama maska
    v.wait(735);                             // wiersz 1: pusty
    v.opm(0x08, 0x78);                       // nowe operatory: retrigger tej samej nuty
    v.wait(735);                             // wiersz 2: 69
    v.opm(0x28, 0x5A);                       // legato: A5 bez key-on
    v.wait(735);                             // wiersz 3: 81
    v.opm(0x08, 0x00);                       // maska 0 = key-off
    v.wait(735);                             // wiersz 4: ===
    v.opm(0x08, 0x78);
    v.wait(735);                             // wiersz 5: nowy key-on (A5)
    v.opm(0x08, 0x00);
    v.opm(0x08, 0x78);                       // key-off + key-on w jednym wierszu = nuta
    v.wait(735);                             // wiersz 6: 81
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 8);
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(1, 0).note == chipstate::kNoteNone);
    CHECK(p.at(2, 0).note == 69);
    CHECK(p.at(3, 0).note == 81);
    CHECK(p.at(4, 0).note == chipstate::kNoteOff);
    CHECK(p.at(5, 0).note == 81);
    CHECK(p.at(6, 0).note == 81);
}

TEST_CASE("chipstate OPM: kanaly 5 i 6 (indeksy 4, 5) - key-on, legato i key-off niezaleznie od innych kanalow")
{
    Fm v(kOpmClockOfs, 3579545);
    v.opm(0x24, 0xC0 | 7).opm(0x25, 0xC0 | 7);
    v.opm(0x2C, 0x4A).opm(0x2D, 0x3E);       // A4 / C-4
    v.opm(0x08, 0x78 | 4);
    v.opm(0x08, 0x78 | 5);
    v.wait(735);
    v.opm(0x2D, 0x4A);                       // legato kanalu 6
    v.wait(735);
    v.opm(0x08, 0x00 | 4);
    v.wait(735);
    v.opm(0x08, 0x00 | 5);
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, 4).note == 69);
    CHECK(p.at(0, 5).note == 60);
    CHECK(p.at(1, 5).note == 69);
    CHECK(p.at(2, 4).note == chipstate::kNoteOff);
    CHECK(p.at(2, 5).note == chipstate::kNoteNone);
    CHECK(p.at(3, 5).note == chipstate::kNoteOff);
}
