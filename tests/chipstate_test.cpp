// Testy modulu chipstate (buildPattern): syntetyczne strumienie z wiadomymi nutami, pliki korpusu
// (tests/data/valid), zgodnosc kolejnosci kanalow z ChannelLayout silnika oraz limit maxRows.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "test_util.hpp"
#include "vgmplay/chipstate.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace {

using namespace vgmplay;
using chipstate::Cell;
using chipstate::Pattern;

constexpr std::uint16_t kClockOfs[48] = {
    0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68,
    0x6C, 0x70, 0x74, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4,
    0xB8, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC};
constexpr std::uint32_t kDual = 0x40000000u;

// Budowniczy syntetycznego VGM 1.71 (naglowek 0x100).
class Vgm {
public:
    Vgm() : hdr_(0x100, 0)
    {
        std::memcpy(hdr_.data(), "Vgm ", 4);
        put32(8, 0x171);
        put32(0x34, 0x100 - 0x34);
        hdr_[0xD6] = 4;   // C352: dzielnik zegara
    }
    Vgm& clock(std::size_t chipIdx, std::uint32_t clk) { put32(kClockOfs[chipIdx], clk); return *this; }
    Vgm& hdrByte(std::size_t ofs, std::uint8_t v) { hdr_[ofs] = v; return *this; }
    Vgm& raw(std::initializer_list<int> b) { for (int v : b) d_.push_back(static_cast<std::uint8_t>(v)); return *this; }
    Vgm& wait(std::uint32_t n)
    {
        total_ += n;
        while (n > 0) {
            const std::uint32_t c = std::min<std::uint32_t>(n, 65535);
            raw({0x61, static_cast<int>(c & 0xFF), static_cast<int>(c >> 8)});
            n -= c;
        }
        return *this;
    }
    Vgm& sn(int v, int inst = 0) { return raw({inst ? 0x30 : 0x50, v}); }
    Vgm& opll(int r, int v, int inst = 0) { return raw({inst ? 0xA1 : 0x51, r, v}); }
    Vgm& ym2612(int port, int r, int v, int inst = 0) { return raw({(inst ? 0xA2 : 0x52) + port, r, v}); }
    Vgm& ym2151(int r, int v) { return raw({0x54, r, v}); }
    Vgm& ym2203(int r, int v) { return raw({0x55, r, v}); }
    Vgm& opl2(int r, int v) { return raw({0x5A, r, v}); }
    Vgm& opl3(int port, int r, int v) { return raw({0x5E + port, r, v}); }
    Vgm& ay(int r, int v, int inst = 0) { return raw({0xA0, r | (inst << 7), v}); }
    Vgm& oki(int r, int v) { return raw({0xB8, r, v}); }
    Vgm& block(int type, const std::vector<std::uint8_t>& payload)
    {
        raw({0x67, 0x66, type});
        const auto n = static_cast<std::uint32_t>(payload.size());
        raw({static_cast<int>(n & 0xFF), static_cast<int>((n >> 8) & 0xFF), static_cast<int>((n >> 16) & 0xFF),
             static_cast<int>(n >> 24)});
        d_.insert(d_.end(), payload.begin(), payload.end());
        return *this;
    }
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

// Testy syntetyczne zakladaja wiersz 735 tickow (1/60 s), niezaleznie od domyslnej wartosci kontraktu.
chipstate::PatternOptions opt735() { chipstate::PatternOptions o; o.rowTicks = 735; return o; }

Pattern build(const std::vector<std::uint8_t>& bytes, chipstate::PatternOptions opt = opt735())
{
    auto h = format::parseVgmHeader(bytes);
    REQUIRE_MESSAGE(h.ok(), (h.ok() ? "" : h.error().message));
    auto s = format::validateCommandStream(bytes, h.value());
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    auto p = chipstate::buildPattern(bytes, h.value(), s.value(), opt);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
    return std::move(p).value();
}

std::vector<int> noteOns(const Pattern& p, std::size_t ch)
{
    std::vector<int> v;
    for (std::uint64_t r = 0; r < p.rowCount; ++r)
        if (p.at(r, ch).note >= 0) v.push_back(p.at(r, ch).note);
    return v;
}
std::size_t noteOffs(const Pattern& p, std::size_t ch)
{
    std::size_t n = 0;
    for (std::uint64_t r = 0; r < p.rowCount; ++r)
        if (p.at(r, ch).note == chipstate::kNoteOff) ++n;
    return n;
}
std::uint64_t rowOfOff(const Pattern& p, std::size_t ch)
{
    for (std::uint64_t r = 0; r < p.rowCount; ++r)
        if (p.at(r, ch).note == chipstate::kNoteOff) return r;
    return ~0ull;
}

const std::vector<int> kOde = {64, 64, 65, 67, 67, 65, 64, 62, 60, 60, 62, 64, 64, 62, 62};
const std::vector<int> kScale = {60, 62, 64, 65, 67, 69, 71, 72};

double hz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

// Zapisy rejestrow jak w tools/vgmgen (te same wzory).
void ymNote(Vgm& v, int ch, int midi, double clk = 7670454.0)
{
    int block = 0, fnum = 0;
    for (block = 0; block < 8; ++block) {
        fnum = static_cast<int>(std::lround(hz(midi) * 144 * (1 << 20) / clk / std::pow(2.0, block - 1)));
        if (fnum < 2048) break;
    }
    v.ym2612(ch / 3, 0xA4 + ch % 3, (block << 3) | (fnum >> 8));
    v.ym2612(ch / 3, 0xA0 + ch % 3, fnum & 0xFF);
}
void ymPatch(Vgm& v, int ch, int mulOp1 = 1, int tlCar = 0)
{
    const int c = ch % 3, port = ch / 3;
    const int offs[4] = {0x00, 0x04, 0x08, 0x0C};   // op1, op3, op2, op4
    for (int o : offs) {
        const bool car = o == 0x08 || o == 0x0C;
        v.ym2612(port, 0x30 + o + c, o == 0 ? mulOp1 : 1);
        v.ym2612(port, 0x40 + o + c, car ? tlCar : 0x28);
        v.ym2612(port, 0x50 + o + c, 0x1F);
        v.ym2612(port, 0x60 + o + c, 0x0A);
        v.ym2612(port, 0x70 + o + c, 0x05);
        v.ym2612(port, 0x80 + o + c, 0x26);
        v.ym2612(port, 0x90 + o + c, 0x00);
    }
    v.ym2612(port, 0xB0 + c, (3 << 3) | 4);
}
int keyCh(int ch) { return ch < 3 ? ch : ch + 1; }

} // namespace

// ---------------------------------------------------------------------------------------------
TEST_CASE("chipstate: SN76489 - ton, glosnosc, key-off, kilka zdarzen w wierszu")
{
    Vgm v;
    v.clock(0, 3579545);
    v.sn(0x8E).sn(0x0F).sn(0x90);          // ton 1: okres 254 = A4, tlumienie 0
    v.wait(735 * 2);
    v.sn(0x94);                            // tlumienie 4 -> sama glosnosc
    v.wait(735);
    v.sn(0x9F);                            // wylaczenie
    v.wait(735);
    v.sn(0x9F).sn(0x90);                   // off + on w jednym wierszu = key-on
    v.wait(735);
    v.sn(0x90).sn(0x9F);                   // on + off w jednym wierszu: nuta zostaje, "===" w nastepnym
    v.wait(735 * 3);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 4);
    CHECK(p.channels[3].name == "Noise");
    CHECK(p.rowCount == (735 * 8) / 735 + 1);
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 0).instrument == 0);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(p.at(1, 0).note == chipstate::kNoteNone);
    CHECK(p.at(2, 0).note == chipstate::kNoteNone);
    CHECK(p.at(2, 0).volume == 47);            // (15-4)/15*64 = 46.9
    CHECK(p.at(3, 0).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: SN76489 - off+on w jednym wierszu daje nute, on+off nute z '===' nastepnie")
{
    Vgm v;
    v.clock(0, 3579545);
    v.sn(0x8E).sn(0x0F).sn(0x90);
    v.wait(735);                           // wiersz 0: nuta
    v.sn(0x9F).sn(0x90);                   // wiersz 1: off+on
    v.wait(735);
    v.sn(0x9F).sn(0x90).sn(0x9F);          // wiersz 2: off, on, off
    v.wait(735 * 2);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, 0).note == 69);
    // off+on w tym samym wierszu = key-on (nuta, bez "===")
    CHECK(p.at(1, 0).note == 69);
    // on+off: nuta zagrana w wierszu 2 zostaje (stan po off -> wylaczony; "===" w wierszu 3)
    CHECK(p.at(3, 0).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: SN76489 - szum (instrument, nuta z szybkosci) i dual chip")
{
    Vgm v;
    v.clock(0, 3579545 | kDual);
    v.sn(0xE5).sn(0xF0);                   // szum bialy (bit 2), szybkosc 1; glosnosc 0
    v.sn(0x8E, 1).sn(0x0F, 1).sn(0x90, 1);  // drugi uklad: ton 1
    v.wait(735);
    v.sn(0xFF);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 8);
    CHECK(p.channels[4].instance == 1);
    CHECK(p.channels[7].channelIdx == 3);
    CHECK(p.at(0, 3).note == 105);         // clk/(32*32) = 3495 Hz -> MIDI 105
    CHECK(p.at(0, 3).instrument == 1);     // bialy
    CHECK(p.at(0, 4).note == 69);
    CHECK(p.at(1, 3).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: YM2612 - nuta, glosnosc nosnych, instrumenty w kolejnosci pojawienia")
{
    Vgm v;
    v.clock(2, 7670454);
    ymPatch(v, 0);
    ymNote(v, 0, 69);
    v.ym2612(0, 0x28, 0xF0);
    v.wait(735 * 2);
    v.ym2612(0, 0x4C, 0x20).ym2612(0, 0x48, 0x20);   // TL nosnych (op2, op4) -> glosnosc
    v.wait(735);
    v.ym2612(0, 0x28, 0x00);
    v.wait(735);
    ymPatch(v, 0, 2);                                // inny patch (MUL op1)
    ymNote(v, 0, 60);
    v.ym2612(0, 0x28, 0xF0);
    v.wait(735);
    v.ym2612(0, 0x28, 0x00);
    ymPatch(v, 0, 1);                                // pierwszy patch z TL nosnych 0 (glosnosc nie wchodzi do klucza)
    ymNote(v, 0, 62);
    v.ym2612(0, 0x28, 0xF0);
    v.wait(735 * 2);
    // FM 4 (port 1)
    ymPatch(v, 3);
    ymNote(v, 3, 72);
    v.ym2612(0, 0x28, 0xF0 | keyCh(3));
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 7);
    CHECK(p.channels[6].name == "DAC");
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 0).instrument == 0);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(p.at(2, 0).note == chipstate::kNoteNone);
    CHECK(p.at(2, 0).volume == 48);                  // (127-32)/127*64 = 47.9
    CHECK(p.at(3, 0).note == chipstate::kNoteOff);
    CHECK(p.at(4, 0).note == 60);
    CHECK(p.at(4, 0).instrument == 1);
    CHECK(p.at(5, 0).note == 62);
    CHECK(p.at(5, 0).instrument == 0);               // ten sam zestaw co pierwszy
    CHECK(p.at(7, 3).note == 72);
    CHECK(p.at(7, 3).instrument == 0);
}

TEST_CASE("chipstate: YM2612 - DAC jako kanal z nuta C-4 (zapisy, 0x8n, strumien 0x90..0x95)")
{
    Vgm v;
    v.clock(2, 7670454);
    v.ym2612(0, 0x2B, 0x80);
    v.ym2612(0, 0x2A, 0x90);                         // wiersz 0: start probki
    for (int i = 0; i < 20; ++i) v.wait(44).ym2612(0, 0x2A, 0x80);   // 880 ticks zapisow
    v.wait(735 * 3);                                  // cisza -> key-off
    // strumien: blok PCM 100 B w banku 0, 4410 Hz => 100 probek = 1000 ticks
    v.block(0x00, std::vector<std::uint8_t>(100, 0x80));
    v.raw({0x90, 0x00, 0x02, 0x00, 0x2A});
    v.raw({0x91, 0x00, 0x00, 0x01, 0x00});
    v.raw({0x92, 0x00, 0x4A, 0x11, 0x00, 0x00});     // 0x114A = 4426 Hz => ok. 1000 ticks (100 probek)
    v.raw({0x93, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00});
    v.wait(735 * 3);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 7);
    CHECK(p.at(0, 6).note == 60);
    CHECK(p.at(0, 6).volume == 0x40);
    CHECK(rowOfOff(p, 6) != ~0ull);
    CHECK(noteOns(p, 6).size() == 2);
    CHECK(noteOffs(p, 6) == 2);
}

TEST_CASE("chipstate: YM2151 - KC/KF, glosnosc nosnych, key-off")
{
    Vgm v;
    v.clock(3, 3579545);
    for (int slot = 0; slot < 4; ++slot) v.ym2151(0x60 + slot * 8, slot >= 2 ? 0x00 : 0x20);
    v.ym2151(0x20, 0xC0 | (3 << 3) | 4);
    v.ym2151(0x28, 0x4A).ym2151(0x30, 0x00);          // A4
    v.ym2151(0x08, 0x78);
    v.wait(735);
    v.ym2151(0x78, 0x20);                             // TL C2 -> tylko (op4) - op2 nadal 0 => bez zmiany glosnosci
    v.ym2151(0x70, 0x20);                             // oba nosne -> glosnosc 48
    v.wait(735);
    v.ym2151(0x28, 0x3E);                             // KC: oktawa 3, kod 14 = C-4 (60)
    v.wait(735);
    v.ym2151(0x08, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 8);
    CHECK(p.channels[0].name == "Ch 1");
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(p.at(1, 0).volume == 48);
    CHECK(p.at(2, 0).note == 60);
    CHECK(p.at(3, 0).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: OKIM6295 - start/stop/koniec probki z tabeli fraz ROM")
{
    // ROM: 8 B na fraze. Fraza 1: 0x000400..0x0004FF => 2*(0x100) = 512 nibbli; zegar 1 MHz, pin7=0 => /165.
    std::vector<std::uint8_t> rom(8 + 1024, 0);
    auto setPhrase = [&](int n, std::uint32_t s, std::uint32_t e) {
        std::uint8_t* t = rom.data() + 8 + n * 8;
        t[0] = static_cast<std::uint8_t>(s >> 16); t[1] = static_cast<std::uint8_t>(s >> 8); t[2] = static_cast<std::uint8_t>(s);
        t[3] = static_cast<std::uint8_t>(e >> 16); t[4] = static_cast<std::uint8_t>(e >> 8); t[5] = static_cast<std::uint8_t>(e);
    };
    setPhrase(1, 0x400, 0x4FF);
    setPhrase(2, 0x600, 0x600);            // nieprawidlowa (start >= stop)
    const std::uint32_t memSize = 0x40000;
    for (int i = 0; i < 4; ++i) { rom[i] = static_cast<std::uint8_t>(memSize >> (8 * i)); rom[4 + i] = 0; }
    Vgm v;
    v.clock(24, 1'000'000);
    v.block(0x8B, rom);
    v.oki(0, 0x81).oki(0, 0x11);           // fraza 1 na glosie 1 (tlumienie 1)
    v.oki(0, 0x82).oki(0, 0x20);           // fraza 2 (nieprawidlowa) na glosie 2 - ignorowana
    v.wait(735 * 3);
    v.oki(0, 0x81).oki(0, 0x40);           // fraza 1 na glosie 3
    v.wait(735);
    v.oki(0, 0x20);                        // stop glosu 2? bit 5 -> glos 3 (bity 3..6 = glosy 1..4)
    v.wait(735 * 2);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 4);
    CHECK(p.at(0, 0).note == 60);
    CHECK(p.at(0, 0).instrument == 1);
    CHECK(p.at(0, 0).volume == 44);        // 0x16/0x20*64
    // dlugosc: 512 nibbli / (1e6/165) Hz = 84.5 ms = 3725 ticks -> wiersz 5
    CHECK(rowOfOff(p, 0) == 5);
    CHECK(noteOns(p, 1).empty());
    CHECK(p.at(3, 2).note == 60);
    CHECK(p.at(4, 2).note == chipstate::kNoteOff);   // stop (bit 5 = glos 3)
}

TEST_CASE("chipstate: AY8910 - ton, szum jako instrument, glosnosc, dual")
{
    Vgm v;
    v.clock(18, 1789773 | kDual);
    v.ay(7, 0x3E).ay(0, 254).ay(1, 0).ay(8, 0x0F);     // A: ton A4
    v.ay(7, 0x3E, 1).ay(2, 254, 1).ay(3, 0, 1).ay(9, 0x0F, 1).ay(7, 0x3D, 1);   // 2. uklad: kanal B
    v.wait(735);
    v.ay(8, 0x08);
    v.wait(735);
    v.ay(7, 0x36);                                      // ton A + szum A
    v.wait(735);
    v.ay(8, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 6);
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 0).instrument == 0);
    CHECK(p.at(1, 0).volume == 34);
    CHECK(p.at(2, 0).note == 69);
    CHECK(p.at(2, 0).instrument == 1);                  // szum wmieszany
    CHECK(p.at(3, 0).note == chipstate::kNoteOff);
    CHECK(p.at(0, 4).note == 69);                       // 2. uklad, kanal B
    CHECK(p.channels[4].instance == 1);
}

TEST_CASE("chipstate: YM2203 - FM i SSG w jednym ukladzie")
{
    Vgm v;
    v.clock(6, 4'000'000);
    // FM 1: fnum 1038 blok 4 => 440 Hz przy sr = 4 MHz / 72
    v.ym2203(0xB0, 0x1C);
    for (int o : {0x0C, 0x08}) v.ym2203(0x40 + o, 0);
    v.ym2203(0xA4, (4 << 3) | (1038 >> 8)).ym2203(0xA0, 1038 & 0xFF);
    v.ym2203(0x28, 0xF0);
    // SSG A: domyslny preskaler: zegar AY = 2 MHz => okres 284
    v.ym2203(7, 0x3E).ym2203(0, 284 & 0xFF).ym2203(1, 284 >> 8).ym2203(8, 0x0F);
    v.wait(735);
    v.ym2203(0x28, 0x00).ym2203(8, 0);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 6);
    CHECK(p.channels[3].name == "AY-3-8910 Ch 1");
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 3).note == 69);
    CHECK(p.at(1, 0).note == chipstate::kNoteOff);
    CHECK(p.at(1, 3).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: YM2413 - instrument z rejestru, glosnosc, perkusja")
{
    Vgm v;
    v.clock(1, 3579545);
    v.opll(0x30, 0x13);                                 // instrument 1, tlumienie 3
    v.opll(0x10, 290 & 0xFF).opll(0x20, 0x10 | (4 << 1) | (290 >> 8));   // A4
    v.wait(735);
    v.opll(0x30, 0x10);                                 // tlumienie 0 -> glosnosc
    v.wait(735);
    v.opll(0x20, 0x00);
    v.wait(735);
    v.opll(0x36, 0x02);                                 // perkusja: BD tlumienie 2
    v.opll(0x0E, 0x30);                                 // tryb rytmu + BD
    v.wait(735);
    v.opll(0x0E, 0x20);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 14);
    CHECK(p.channels[9].name == "Bass Drum");
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(0, 0).instrument == 1);
    CHECK(p.at(0, 0).volume == 51);                     // (15-3)/15*64
    CHECK(p.at(1, 0).volume == 64);
    CHECK(p.at(2, 0).note == chipstate::kNoteOff);
    CHECK(p.at(3, 9).note >= 0);
    CHECK(p.at(3, 9).volume == 55);                     // 13/15*64 = 55.5 -> 55 lub 56
    CHECK(p.at(4, 9).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate: YM3812 / YMF262 - nuta, glosnosc, perkusja, 2. bank OPL3")
{
    {
        Vgm v;
        v.clock(9, 3579545);
        v.opl2(0x20, 0x21).opl2(0x23, 0x21).opl2(0x43, 0x00).opl2(0x40, 0x10).opl2(0xC0, 0x04);
        v.opl2(0xA0, 580 & 0xFF).opl2(0xB0, 0x20 | (4 << 2) | (580 >> 8));
        v.wait(735);
        v.opl2(0x43, 0x10);
        v.wait(735);
        v.opl2(0xB0, 0x00);
        v.wait(735);
        v.opl2(0x53, 0x00);                              // BD: TL nosnej kanalu 7
        v.opl2(0xBD, 0x30);
        v.wait(735);
        v.opl2(0xBD, 0x20);
        v.wait(735);
        const Pattern p = build(v.bytes());
        REQUIRE(p.channels.size() == 14);
        CHECK(p.at(0, 0).note == 69);
        CHECK(p.at(0, 0).volume == 0x40);
        CHECK(p.at(1, 0).volume == 48);                  // (63-16)/63*64
        CHECK(p.at(2, 0).note == chipstate::kNoteOff);
        CHECK(p.at(3, 9).note >= 0);
        CHECK(p.at(4, 9).note == chipstate::kNoteOff);
    }
    {
        Vgm v;
        v.clock(12, 14318180);
        v.opl3(1, 0x05, 0x01);
        // kanal 10 (bank 1, kanal 0): nuta A4 przy sr = 14318180/288
        const int fn = static_cast<int>(std::lround(440.0 * (1 << 16) / (14318180 / 288.0)));
        v.opl3(1, 0x23, 0x21).opl3(1, 0xC0, 0x34);
        v.opl3(1, 0xA0, fn & 0xFF).opl3(1, 0xB0, 0x20 | (4 << 2) | (fn >> 8));
        v.wait(735);
        v.opl3(1, 0xB0, 0x00);
        v.wait(735);
        const Pattern p = build(v.bytes());
        REQUIRE(p.channels.size() == 23);
        CHECK(p.at(0, 9).note == 69);
        CHECK(p.at(1, 9).note == chipstate::kNoteOff);
        CHECK(noteOns(p, 0).empty());
    }
}

TEST_CASE("chipstate: limit maxRows obcina i ostrzega")
{
    Vgm v;
    v.clock(0, 3579545);
    v.sn(0x8E).sn(0x0F).sn(0x90);
    v.wait(735 * 100);
    chipstate::PatternOptions o;
    o.maxRows = 10;
    const Pattern p = build(v.bytes(), o);
    CHECK(p.rowCount == 10);
    CHECK(p.cells.size() == 10 * p.channels.size());
    CHECK(p.at(0, 0).note == 69);
    REQUIRE_FALSE(p.warnings.empty());
    CHECK(p.warnings[0].code == WarningCode::LimitReached);
    // rowTicks z opcji
    chipstate::PatternOptions o2;
    o2.rowTicks = 882;
    const Pattern p2 = build(v.bytes(), o2);
    CHECK(p2.rowTicks == 882);
    CHECK(p2.rowCount == (735u * 100) / 882 + 1);
}

// ---------------------------------------------------------------------------------------------
// Korpus (tools/vgmgen): nuty generatora = oczekiwane nuty we wzorcu.
// ---------------------------------------------------------------------------------------------
namespace {

struct Corpus {
    loader::LoadedTrack track;
    Pattern pat;
};
Corpus loadCorpus(const char* name)
{
    auto t = loader::loadTrackFromFile(testutil::dataDir() / "valid" / name);
    REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
    Corpus c{std::move(t).value(), {}};
    auto p = chipstate::buildPattern(c.track.raw(), c.track.header, c.track.stream, opt735());
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? "" : p.error().message));
    c.pat = std::move(p).value();
    return c;
}
std::vector<int> shifted(std::vector<int> v, int s)
{
    for (int& x : v) x += s;
    return v;
}

} // namespace

TEST_CASE("chipstate korpus: gama SN76489 i melodia YM2612 (Oda do radosci)")
{
    {
        const Corpus c = loadCorpus("sn76489_scale_v100.vgm");
        CHECK(noteOns(c.pat, 0) == kScale);
        CHECK(noteOffs(c.pat, 0) == kScale.size());
        CHECK(c.pat.at(0, 0).volume == 60);   // generator: tlumienie 1 -> 14/15*64
        CHECK(c.pat.rowCount == c.track.stream.totalSamples / 735 + 1);
    }
    {
        const Corpus c = loadCorpus("ym2612_melody_loop_v150_gd3jp.vgm");
        REQUIRE(c.pat.channels.size() == 7);
        CHECK(noteOns(c.pat, 0) == kOde);
        CHECK(noteOffs(c.pat, 0) == kOde.size());
        for (std::size_t ch = 1; ch < 7; ++ch) CHECK(noteOns(c.pat, ch).empty());
        // petla: wstep 2205 ticks (3 wiersze) przed melodia
        CHECK(c.pat.loopStartRow == c.track.stream.loopStartSample / 735);
        CHECK(c.pat.loopStartRow == 3);
    }
}

TEST_CASE("chipstate korpus: pozostale uklady grajace melodie generatora")
{
    {   // Mega Drive: SN76489 (kanaly 0..3) + YM2612 (4..10); SN gra +12
        const Corpus c = loadCorpus("megadrive_ym2612_sn76489_v151.vgz");
        REQUIRE(c.pat.channels.size() == 11);
        CHECK(noteOns(c.pat, 0) == shifted(kOde, 12));
        CHECK(noteOns(c.pat, 4) == kOde);
    }
    {   // dual: YM2612 x2 + SN76489 x2; kolejnosc wg naglowka: SN(0,1), YM2612(0,1)
        const Corpus c = loadCorpus("dual_chip_ym2612_sn76489_v151.vgm");
        REQUIRE(c.pat.channels.size() == 4 + 4 + 7 + 7);
        CHECK(noteOns(c.pat, 0) == shifted(kOde, 12));
        CHECK(noteOns(c.pat, 4) == shifted(kOde, 19));
        CHECK(noteOns(c.pat, 8) == kOde);
        CHECK(noteOns(c.pat, 15) == shifted(kOde, 7));
    }
    {
        const Corpus c = loadCorpus("ay8910_melody_loop_v151_gd3en.vgm");
        CHECK(noteOns(c.pat, 0) == kOde);
    }
    {
        const Corpus c = loadCorpus("ym2151_melody_loop_v161_gd3jp.vgm");
        CHECK(noteOns(c.pat, 0) == kOde);
    }
    {
        const Corpus c = loadCorpus("ym3812_melody_v151.vgm");
        CHECK(noteOns(c.pat, 0) == kOde);
    }
    {
        const Corpus c = loadCorpus("ymf262_melody_loop_v170.vgm");
        CHECK(noteOns(c.pat, 0) == kOde);
    }
    {
        const Corpus c = loadCorpus("ym2413_melody_loop_v171_gd3jp.vgm");
        CHECK(noteOns(c.pat, 0) == kOde);
    }
}

TEST_CASE("chipstate korpus: wszystkie pliki valid/ buduja wzorzec o spojnych rozmiarach")
{
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(testutil::dataDir() / "valid")) {
        const auto ext = e.path().extension();
        if (ext != ".vgm" && ext != ".vgz") continue;
        INFO(e.path().filename().string());
        auto t = loader::loadTrackFromFile(e.path());
        REQUIRE(t.ok());
        auto p = chipstate::buildPattern(t.value().raw(), t.value().header, t.value().stream);
        REQUIRE(p.ok());
        const Pattern& pat = p.value();
        CHECK(pat.rowCount == t.value().stream.totalSamples / pat.rowTicks + 1);
        CHECK(pat.cells.size() == pat.rowCount * pat.channels.size());
        CHECK(pat.loopStartRow < pat.rowCount);
        ++files;
    }
    CHECK(files >= 15);
}

// ---------------------------------------------------------------------------------------------
// Kolejnosc kanalow == ChannelLayout silnika.
// ---------------------------------------------------------------------------------------------
namespace {

struct Flat {
    ChipType type; std::uint8_t instance; std::uint16_t idx; std::string name;
    bool operator==(const Flat&) const = default;
};
std::vector<Flat> flatten(const ChannelLayout& l)
{
    std::vector<Flat> v;
    for (const DeviceDesc& d : l.devices)
        for (std::uint16_t i = 0; i < d.channelCount; ++i) v.push_back({d.type, d.instance, i, d.channelNames[i]});
    return v;
}
std::vector<Flat> flatten(const Pattern& p)
{
    std::vector<Flat> v;
    for (const auto& c : p.channels) v.push_back({c.type, c.instance, c.channelIdx, c.name});
    return v;
}
void expectSameOrder(const std::vector<std::uint8_t>& bytes)
{
    auto dec = engine::makeVgmDecoder();
    DecoderConfig cfg;
    cfg.sampleRate = 44100;
    auto r = dec->open(bytes, cfg);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const Pattern p = build(bytes);
    const auto a = flatten(r.value().layout), b = flatten(p);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        INFO("kanal " << i << " silnik: " << a[i].name << " / wzorzec: " << b[i].name);
        CHECK(a[i] == b[i]);
    }
}

} // namespace

TEST_CASE("chipstate: kolejnosc kanalow wzorca == ChannelLayout silnika (pliki valid/)")
{
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(testutil::dataDir() / "valid")) {
        const auto ext = e.path().extension();
        if (ext != ".vgm" && ext != ".vgz") continue;
        INFO(e.path().filename().string());
        auto t = loader::loadTrackFromFile(e.path());
        REQUIRE(t.ok());
        expectSameOrder(std::vector<std::uint8_t>(t.value().raw().begin(), t.value().raw().end()));
        ++files;
    }
    CHECK(files >= 15);
}

TEST_CASE("chipstate: kolejnosc kanalow == ChannelLayout dla kazdego z 48 ukladow naglowka (+C219, NES FDS, dual)")
{
    for (std::size_t idx = 0; idx < 48; ++idx) {
        INFO("indeks ukladu w naglowku: " << idx);
        Vgm v;
        v.clock(idx, 4'000'000);
        v.wait(10);
        expectSameOrder(v.bytes());
    }
    {   // C219: C140 z bajtem 0x96 == 2
        Vgm v;
        v.clock(28, 4'000'000).hdrByte(0x96, 2).wait(10);
        expectSameOrder(v.bytes());
    }
    {   // NES z FDS (bit 31 zegara)
        Vgm v;
        v.clock(20, 1'789'772 | 0x80000000u).wait(10);
        expectSameOrder(v.bytes());
    }
    {   // kilka ukladow, w tym podwojne i z ukladem polaczonym
        Vgm v;
        v.clock(0, 3579545 | kDual).clock(2, 7670454).clock(6, 4'000'000 | kDual).clock(12, 14318180)
            .clock(24, 1'000'000).clock(18, 1789773 | kDual).wait(10);
        expectSameOrder(v.bytes());
    }
}

// =============================================================================================
// NES APU / FDS, C140, K054539, K053260, Sega PCM, GA20, OKIM6258.
// =============================================================================================
namespace {

Vgm& nes(Vgm& v, int reg, int val, int inst = 0) { return v.raw({0xB4, reg | (inst << 7), val}); }
// FDS: rejestr $40xx/$408x w komendzie 0xB4 (mapowanie libvgm: $4080+n = 0x20+n, $4023 = 0x3F).
Vgm& fds(Vgm& v, int addr, int val) { return nes(v, addr == 0x4023 ? 0x3F : (addr >= 0x4080 ? 0x20 + (addr - 0x4080) : addr - 0x4000), val); }
Vgm& segapcm(Vgm& v, int off, int val, int inst = 0) { return v.raw({0xC0, off & 0xFF, ((off >> 8) & 0x7F) | (inst << 7), val}); }
Vgm& k054539(Vgm& v, int addr, int val, int inst = 0) { return v.raw({0xD3, ((addr >> 8) & 0x7F) | (inst << 7), addr & 0xFF, val}); }
Vgm& c140(Vgm& v, int addr, int val) { return v.raw({0xD4, (addr >> 8) & 0x7F, addr & 0xFF, val}); }
Vgm& k053260(Vgm& v, int reg, int val) { return v.raw({0xBA, reg, val}); }
Vgm& ga20(Vgm& v, int reg, int val) { return v.raw({0xBF, reg, val}); }
Vgm& okim6258(Vgm& v, int reg, int val) { return v.raw({0xB7, reg, val}); }

std::vector<std::uint8_t> romPayload(std::uint32_t memSize, std::uint32_t start, const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> p;
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<std::uint8_t>(memSize >> (8 * i)));
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<std::uint8_t>(start >> (8 * i)));
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

void nesTimer(Vgm& v, int regLo, int midi, int lenIdx, double div = 16.0)   // $4002/$4003, $4006/$4007, $400A/$400B
{
    const int t = static_cast<int>(std::lround(1789773.0 / (div * hz(midi)))) - 1;
    nes(v, regLo, t & 0xFF);
    nes(v, regLo + 1, ((t >> 8) & 7) | (lenIdx << 3));
}

constexpr std::size_t kIdxSegaPcm = 4, kIdxNes = 20, kIdxOkim6258 = 23, kIdxK054539 = 26, kIdxC140 = 28, kIdxK053260 = 29,
                      kIdxGa20 = 40;

} // namespace


namespace {
std::vector<std::uint64_t> offRows(const Pattern& p, std::size_t ch)
{
    std::vector<std::uint64_t> v;
    for (std::uint64_t r = 0; r < p.rowCount; ++r)
        if (p.at(r, ch).note == chipstate::kNoteOff) v.push_back(r);
    return v;
}
std::vector<int> instrumentsAtNotes(const Pattern& p, std::size_t ch)
{
    std::vector<int> v;
    for (std::uint64_t r = 0; r < p.rowCount; ++r)
        if (p.at(r, ch).note >= 0) v.push_back(p.at(r, ch).instrument);
    return v;
}
} // namespace

TEST_CASE("chipstate NES: uklad i kanaly (Square 1/2, Triangle, Noise, DPCM) sa zdekodowane")
{
    Vgm v;
    v.clock(kIdxNes, 1789773).wait(10);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 5);
    const char* names[5] = {"Square 1", "Square 2", "Triangle", "Noise", "DPCM"};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(p.channels[i].name == names[i]);
        CHECK(p.channels[i].decoded);
    }
}

TEST_CASE("chipstate NES: pulse - nuta z timera, duty, glosnosc, key-off z $4015, kanal 2 niezalezny")
{
    Vgm v;
    v.clock(kIdxNes, 1789773);
    nes(v, 0x15, 0x0F);
    nes(v, 0x00, 0xBF);   // duty 2, halt (petla), stala glosnosc 15
    nes(v, 0x01, 0x08);   // sweep: negate, bez przepelnienia
    for (int m : kScale) { nesTimer(v, 0x02, m, 1); v.wait(735); }
    nes(v, 0x15, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(noteOns(p, 0) == kScale);
    CHECK(p.at(0, 0).instrument == 2);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(rowOfOff(p, 0) == 8);
    CHECK(noteOns(p, 1).empty());
}

TEST_CASE("chipstate NES: pulse - sweep wycisza przy przepelnieniu, koniec z licznika dlugosci i obwiedni")
{
    {   // niska nuta (C-2): bez negate cel = 2 * timer >= 0x800 wycisza kanal, z negate - nie
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x0F);
        nes(v, 0x00, 0xBF);
        nes(v, 0x01, 0x00);
        nesTimer(v, 0x02, 36, 1);
        v.wait(735);
        nes(v, 0x01, 0x08);
        nesTimer(v, 0x02, 36, 1);
        v.wait(735);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 0).note < 0);
        CHECK(p.at(1, 0).note == 36);
    }
    {   // licznik dlugosci: indeks 3 = 2 polklatki (120 Hz) = 736 tickow => key-off w wierszu 1
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x0F);
        nes(v, 0x00, 0x1F);   // bez halt, stala glosnosc 15
        nesTimer(v, 0x02, 60, 3);
        v.wait(735 * 4);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 0).note == 60);
        CHECK(offRows(p, 0) == std::vector<std::uint64_t>{1});
    }
    {   // obwiednia sprzetowa (bez petli) wygasa po (1 + 15 * (v + 1)) cwiartkach klatki = 11210 tickow => wiersz 15
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x0F);
        nes(v, 0x00, 0x03);   // obwiednia, v = 3
        nesTimer(v, 0x02, 60, 1);
        v.wait(735 * 20);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 0).volume == 0x40);
        CHECK(offRows(p, 0) == std::vector<std::uint64_t>{15});
    }
    {   // glosnosc 0 przy stalej glosnosci = cisza; potem glosnosc 8 = nuta z glosnoscia 8/15
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x0F);
        nes(v, 0x00, 0xB0);
        nesTimer(v, 0x02, 60, 1);
        v.wait(735);
        nes(v, 0x00, 0xB8);
        v.wait(735);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 0).note < 0);
        CHECK(p.at(1, 0).note == 60);
        CHECK(p.at(1, 0).volume == 34);   // 8/15 * 64 = 34.1
    }
}

TEST_CASE("chipstate NES: triangle - nuta (timer, f = clock / 32 / (t + 1)), licznik liniowy, key-off")
{
    {
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x04);
        nes(v, 0x08, 0xFF);   // kontrola (sustain) + przeladowanie 127
        for (int m : kScale) { nesTimer(v, 0x0A, m, 1, 32.0); v.wait(735); }
        nes(v, 0x08, 0x80);   // przeladowanie 0 => cisza
        v.wait(735);
        const Pattern p = build(v.bytes());
        CHECK(noteOns(p, 2) == kScale);
        CHECK(p.at(0, 2).volume == 0x40);
        CHECK(rowOfOff(p, 2) == 8);
    }
    {   // bez kontroli: licznik liniowy 4 => (1 + 4) cwiartek = 919 tickow => wiersz 1
        Vgm v;
        v.clock(kIdxNes, 1789773);
        nes(v, 0x15, 0x04);
        nes(v, 0x08, 0x04);
        nesTimer(v, 0x0A, 57, 1, 32.0);
        v.wait(735 * 4);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 2).note == 57);
        CHECK(offRows(p, 2) == std::vector<std::uint64_t>{1});
    }
}

TEST_CASE("chipstate NES: noise - nuta z okresu, instrument = tryb, key-off")
{
    Vgm v;
    v.clock(kIdxNes, 1789773);
    nes(v, 0x15, 0x08);
    nes(v, 0x0C, 0x3F);   // halt, stala glosnosc 15
    nes(v, 0x0E, 0x06);   // okres 128 => 1789773 / (32 * 128) = 437 Hz = A-4
    nes(v, 0x0F, 0x08);
    v.wait(735);
    nes(v, 0x0E, 0x86);   // szum krotki
    nes(v, 0x0F, 0x08);
    v.wait(735);
    nes(v, 0x15, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(noteOns(p, 3) == std::vector<int>{69, 69});
    CHECK(instrumentsAtNotes(p, 3) == std::vector<int>{1, 2});
    CHECK(p.at(0, 3).volume == 0x40);
    CHECK(offRows(p, 3) == std::vector<std::uint64_t>{2});
}

TEST_CASE("chipstate NES: DPCM - start z $4015, instrument = unikatowa probka, koniec z dlugosci, petla")
{
    Vgm v;
    v.clock(kIdxNes, 1789773);
    nes(v, 0x10, 0x0F);   // tempo 15 (najszybsze), bez petli
    nes(v, 0x12, 0x10);   // adres $C400
    nes(v, 0x13, 0x0F);   // 241 bajtow => 2566 tickow
    nes(v, 0x15, 0x1F);
    v.wait(735 * 5);
    nes(v, 0x15, 0x1F);   // ta sama probka
    v.wait(735 * 5);
    nes(v, 0x12, 0x20);   // nowa probka
    nes(v, 0x15, 0x1F);
    v.wait(735 * 5);
    nes(v, 0x10, 0x00);   // tempo 0 (428 cykli/bit) + petla
    nes(v, 0x10, 0x40);
    nes(v, 0x15, 0x1F);   // poprzednia jeszcze nie trwa, wiec startuje petla
    v.wait(735 * 5);
    nes(v, 0x15, 0x0F);   // wylaczenie DMC => key-off
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(noteOns(p, 4) == std::vector<int>{60, 60, 60, 60 - 36});   // tempo 0 = 4181 Hz = 3 oktawy nizej
    CHECK(instrumentsAtNotes(p, 4) == std::vector<int>{0, 0, 1, 1});
    CHECK(offRows(p, 4) == std::vector<std::uint64_t>{3, 8, 13, 20});
}

TEST_CASE("chipstate NES: FDS - kanal 6, nuta z rejestru 12-bit, instrument = tablica fali, glosnosc, halt")
{
    Vgm v;
    v.clock(kIdxNes, 1789773 | 0x80000000u);
    fds(v, 0x4023, 0x02);                              // I/O wlaczone
    fds(v, 0x4089, 0x80);                              // zapis tablicy fali
    for (int i = 0; i < 64; ++i) nes(v, 0x40 + i, i & 0x3F);
    fds(v, 0x4089, 0x00);
    fds(v, 0x4080, 0xA0);                              // bez obwiedni, glosnosc 32 (max)
    fds(v, 0x4082, 516 & 0xFF);                        // 516 * 1789773 / 2^22 = 220.2 Hz = A-3
    fds(v, 0x4083, 516 >> 8);
    v.wait(735);
    fds(v, 0x4083, 0x80 | (516 >> 8));                 // halt
    v.wait(735);
    fds(v, 0x4089, 0x80);                              // inna tablica fali
    nes(v, 0x40, 0x3F);
    fds(v, 0x4089, 0x00);
    fds(v, 0x4080, 0x90);                              // glosnosc 16 (polowa)
    fds(v, 0x4083, 516 >> 8);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 6);
    CHECK(p.channels[5].name == "FDS");
    CHECK(p.channels[5].decoded);
    CHECK(noteOns(p, 5) == std::vector<int>{57, 57});
    CHECK(instrumentsAtNotes(p, 5) == std::vector<int>{0, 1});
    CHECK(p.at(0, 5).volume == 0x40);
    CHECK(p.at(2, 5).volume == 32);
    CHECK(offRows(p, 5) == std::vector<std::uint64_t>{1});
}

TEST_CASE("chipstate YM2413 z bitem VRC7 (bit 31 zegara): 6 kanalow FM dekodowanych jak OPLL")
{
    Vgm v;
    v.clock(1, 3579545 | 0x80000000u);
    v.opll(0x30, 0x10);
    v.opll(0x10, 290 & 0xFF).opll(0x20, 0x10 | (4 << 1) | (290 >> 8));   // A4
    v.wait(735);
    v.opll(0x20, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 14);
    CHECK(p.at(0, 0).note == 69);
    CHECK(p.at(1, 0).note == chipstate::kNoteOff);
}

TEST_CASE("chipstate K054539: nuta z wartosci 24-bit, koniec z ROM (0x80), petla, glosnosc, key-off, dual")
{
    std::vector<std::uint8_t> rom(0x1000, 0x20);
    rom[2000] = 0x80;   // znacznik konca probki PCM 8-bit
    Vgm v;
    v.clock(kIdxK054539, 18'432'000 | kDual);   // zegar >= 1 MHz: rate = clock / 384 = 48000 Hz
    v.block(0x8C, romPayload(0x1000, 0, rom));
    auto setup = [&](int ch, int delta, int start, int vol, int loop, int inst = 0) {
        const int b = ch * 0x20;
        for (int k = 0; k < 3; ++k) k054539(v, b + k, (delta >> (8 * k)) & 0xFF, inst);
        k054539(v, b + 3, vol, inst);
        for (int k = 0; k < 3; ++k) k054539(v, b + 0x0C + k, (start >> (8 * k)) & 0xFF, inst);
        k054539(v, 0x200 + 2 * ch, 0x00, inst);   // PCM 8 bit
        k054539(v, 0x201 + 2 * ch, loop, inst);
    };
    k054539(v, 0x22F, 0x01);
    setup(0, 0x10000, 0, 0, 0);
    k054539(v, 0x214, 0x01);                       // wiersz 0: 2000 krokow / 48 kHz = 1838 tickow => koniec w wierszu 2
    v.wait(735 * 5);
    setup(1, 0x20000, 0x100, 0, 0);
    k054539(v, 0x214, 0x02);                       // wiersz 5: 1900 krokow po 2 probki => 873 ticki => wiersz 6
    v.wait(735 * 5);
    setup(2, 0x10000, 0, 0x40, 1);                 // petla; glosnosc 0x40 = -36 dB
    k054539(v, 0x214, 0x04);                       // wiersz 10
    v.wait(735 * 3);
    k054539(v, 2 * 0x20 + 3, 0x00);                // wiersz 13: pelna glosnosc
    v.wait(735 * 3);
    k054539(v, 0x215, 0x04);                       // wiersz 16: key-off
    v.wait(735 * 2);
    k054539(v, 0x22F, 0x01, 1);                    // wiersz 18: drugi uklad (kanaly 8..15), bez ROM => bez konca
    setup(0, 0x10000, 0, 0, 0, 1);
    k054539(v, 0x214, 0x01, 1);
    v.wait(735 * 3);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 16);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(noteOns(p, 0) == std::vector<int>{60});
    CHECK(p.at(0, 0).instrument == 0);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(offRows(p, 0) == std::vector<std::uint64_t>{2});
    CHECK(noteOns(p, 1) == std::vector<int>{72});
    CHECK(p.at(5, 1).instrument == 1);
    CHECK(offRows(p, 1) == std::vector<std::uint64_t>{6});
    CHECK(noteOns(p, 2) == std::vector<int>{60});
    CHECK(p.at(10, 2).volume == 1);
    CHECK(p.at(13, 2).volume == 0x40);
    CHECK(offRows(p, 2) == std::vector<std::uint64_t>{16});
    CHECK(noteOns(p, 8) == std::vector<int>{60});
    CHECK(offRows(p, 8).empty());
}

TEST_CASE("chipstate K053260: nuta z pitch, dlugosc, petla/ADPCM, glosnosc, key-off")
{
    Vgm v;
    v.clock(kIdxK053260, 3579545);
    k053260(v, 0x2F, 0x02);
    k053260(v, 0x2A, 0x22);                        // glos 2: petla + ADPCM
    const int g0[8] = {0xC0, 0x0F, 0xE8, 0x03, 0x00, 0x10, 0x00, 0x7F};   // pitch 0xFC0 (ratio 1), dlugosc 1000, start 0x1000
    for (int i = 0; i < 8; ++i) k053260(v, 0x08 + i, g0[i]);
    k053260(v, 0x28, 0x01);
    v.wait(735 * 3);
    const int g1[8] = {0xE0, 0x0F, 0xE8, 0x03, 0x00, 0x20, 0x00, 0x40};   // pitch 0xFE0 (ratio 2), start 0x2000
    for (int i = 0; i < 8; ++i) k053260(v, 0x10 + i, g1[i]);
    k053260(v, 0x28, 0x03);
    v.wait(735 * 3);
    k053260(v, 0x28, 0x00);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 4);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(noteOns(p, 0) == std::vector<int>{60});
    CHECK(p.at(0, 0).instrument == 0);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(offRows(p, 0) == std::vector<std::uint64_t>{1});   // 1000 / (3579545 / 64) = 17.9 ms = 789 tickow
    CHECK(noteOns(p, 1) == std::vector<int>{72});
    CHECK(p.at(3, 1).instrument == 1);
    CHECK(p.at(3, 1).volume == 32);
    CHECK(offRows(p, 1) == std::vector<std::uint64_t>{6});   // petla: do key-off
}

TEST_CASE("chipstate Sega PCM: nuta z delta, koniec z adresu, petla, skok adresu, glosnosc, key-off")
{
    Vgm v;
    v.clock(kIdxSegaPcm, 4'000'000);                // rate = 31250 Hz
    segapcm(v, 2, 0x7F);
    segapcm(v, 3, 0x40);
    segapcm(v, 7, 0x80);                            // delta 0x80 = 0.5 probki na probke => C-3
    segapcm(v, 6, 0x02);                            // strona koncowa 2 => koniec przy adresie 0x30000
    segapcm(v, 0x84, 0x00);
    segapcm(v, 0x85, 0x01);                         // start 0x010000
    segapcm(v, 0x86, 0x02);                         // key-on, petla wylaczona: 0x20000 / 0x80 = 1024 probki = 1446 tickow
    v.wait(735 * 4);
    segapcm(v, 0x86, 0x00);                         // wiersz 4: ten sam adres, petla wlaczona
    v.wait(735 * 2);
    segapcm(v, 0x85, 0x03);                         // wiersz 6: skok adresu w trakcie gry = nowa probka
    v.wait(735);
    segapcm(v, 2, 0x20);                            // wiersz 7: glosnosc max(0x20, 0x40) = 0x40
    v.wait(735);
    segapcm(v, 0x86, 0x01);                         // wiersz 8: key-off
    v.wait(735 * 2);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 16);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(noteOns(p, 0) == std::vector<int>{48, 48, 48});
    CHECK(instrumentsAtNotes(p, 0) == std::vector<int>{0, 0, 1});
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(p.at(7, 0).volume == 32);
    CHECK(offRows(p, 0) == std::vector<std::uint64_t>{1, 8});
    CHECK(noteOns(p, 1).empty());
}

TEST_CASE("chipstate C140: nuta z czestotliwosci, koniec z adresow, key-on bez czestotliwosci, petla")
{
    Vgm v;
    v.clock(kIdxC140, 12'288'000);                  // rate = 42666.7 Hz
    auto voice = [&](int ch, int vol, int freq, int endHi) {
        const int b = ch * 0x10;
        c140(v, b + 0, vol);
        c140(v, b + 1, vol);
        c140(v, b + 2, freq >> 8);
        c140(v, b + 3, freq & 0xFF);
        c140(v, b + 4, 0x00);                       // bank
        c140(v, b + 6, 0x00);
        c140(v, b + 7, 0x00);                       // start 0
        c140(v, b + 8, endHi);
        c140(v, b + 9, 0x00);                       // koniec = endHi << 8
    };
    voice(0, 0xFF, 0x8000, 0x04);                   // ratio 0.5 => C-3; 1024 probki
    c140(v, 0x05, 0x80);                            // wiersz 0: 1024 * 2 / 42666 = 2117 tickow => wiersz 2
    v.wait(735 * 4);
    voice(1, 0x80, 0x0000, 0x04);                   // wiersz 4: key-on bez czestotliwosci
    c140(v, 0x15, 0x80);
    v.wait(735 * 2);
    c140(v, 0x12, 0x40);                            // wiersz 6: czestotliwosc 0x4000 => ratio 0.25 => C-2 (36)
    v.wait(735 * 8);
    voice(2, 0xFF, 0x2000, 0x04);                   // wiersz 14: petla
    c140(v, 0x25, 0x90);
    v.wait(735 * 3);
    c140(v, 0x25, 0x10);                            // wiersz 17: key-off
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 24);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(noteOns(p, 0) == std::vector<int>{48});
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(offRows(p, 0) == std::vector<std::uint64_t>{2});
    CHECK(p.at(4, 1).note < 0);
    CHECK(p.at(6, 1).note == 36);
    CHECK(p.at(6, 1).volume == 32);
    CHECK(offRows(p, 1) == std::vector<std::uint64_t>{11});   // 1024 * 4 / 42666 = 4234 tickow
    CHECK(noteOns(p, 2) == std::vector<int>{24});
    CHECK(offRows(p, 2) == std::vector<std::uint64_t>{17});
}

TEST_CASE("chipstate OKIM6258: start/stop, nuta z dzielnika zegara, strumien DAC (instrument, koniec)")
{
    {
        Vgm v;
        v.clock(kIdxOkim6258, 4'000'000).hdrByte(0x94, 2);   // dzielnik 512 => C-4
        okim6258(v, 0, 0x02);
        v.wait(735 * 2);
        okim6258(v, 0, 0x01);
        v.wait(735);
        const Pattern p = build(v.bytes());
        REQUIRE(p.channels.size() == 1);
        CHECK(p.channels[0].decoded);
        CHECK(p.at(0, 0).note == 60);
        CHECK(p.at(0, 0).volume == 0x40);
        CHECK(offRows(p, 0) == std::vector<std::uint64_t>{2});
    }
    {
        Vgm v;
        v.clock(kIdxOkim6258, 4'000'000).hdrByte(0x94, 0);   // dzielnik 1024 => C-3
        okim6258(v, 0, 0x02);
        v.wait(735);
        const Pattern p = build(v.bytes());
        CHECK(p.at(0, 0).note == 48);
    }
    {   // strumien: blok danych 0x04 (1000 bajtow) po 15625 Hz => 2822 ticki => key-off w wierszu 3
        Vgm v;
        v.clock(kIdxOkim6258, 4'000'000).hdrByte(0x94, 2);
        v.block(0x04, std::vector<std::uint8_t>(1000, 0x11));
        v.raw({0x90, 0x00, 0x17, 0x00, 0x01});
        v.raw({0x91, 0x00, 0x04, 0x01, 0x00});
        v.raw({0x92, 0x00, 0x09, 0x3D, 0x00, 0x00});
        okim6258(v, 0, 0x02);
        v.raw({0x95, 0x00, 0x00, 0x00, 0x00});
        v.wait(735 * 5);
        okim6258(v, 0, 0x01);
        v.wait(735);
        const Pattern p = build(v.bytes());
        CHECK(noteOns(p, 0) == std::vector<int>{60});
        CHECK(p.at(0, 0).instrument == 0);
        CHECK(offRows(p, 0) == std::vector<std::uint64_t>{3});
    }
}

TEST_CASE("chipstate GA20: nuta z tempa, koniec z bajtu 0 w ROM, glosnosc, instrument = adres startu")
{
    std::vector<std::uint8_t> rom(20001, 0x40);
    rom[20000] = 0x00;                              // znacznik konca
    Vgm v;
    v.clock(kIdxGa20, 3579545);                     // rate = 894886 Hz
    v.block(0x93, romPayload(0x8000, 0, rom));
    ga20(v, 0, 0);
    ga20(v, 1, 0);
    ga20(v, 4, 0xFF);                               // 1 probka na krok => ratio 1 => C-4
    ga20(v, 5, 0xF6);
    ga20(v, 6, 0x02);                               // wiersz 0: 20000 / 894886 = 22 ms = 986 tickow => wiersz 1
    v.wait(735 * 3);
    ga20(v, 8, 0x10);                               // start 0x100
    ga20(v, 9, 0x00);
    ga20(v, 12, 0xFE);                              // 2 probki na krok => ratio 0.5 => C-3
    ga20(v, 13, 0xF6);
    ga20(v, 14, 0x02);                              // wiersz 3: 19900 * 2 / 894886 = 1962 ticki => wiersz 5
    v.wait(735 * 4);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 4);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(noteOns(p, 0) == std::vector<int>{60});
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(offRows(p, 0) == std::vector<std::uint64_t>{1});
    CHECK(noteOns(p, 1) == std::vector<int>{48});
    CHECK(p.at(3, 1).instrument == 1);
    CHECK(offRows(p, 1) == std::vector<std::uint64_t>{5});
}

TEST_CASE("chipstate: nowe uklady (NES, C140, K054539, K053260, Sega PCM, GA20, OKIM6258) maja wszystkie kanaly decoded")
{
    for (std::size_t idx : {kIdxSegaPcm, kIdxNes, kIdxOkim6258, kIdxK054539, kIdxC140, kIdxK053260, kIdxGa20}) {
        INFO("indeks ukladu w naglowku: " << idx);
        Vgm v;
        v.clock(idx, 4'000'000).wait(10);
        const Pattern p = build(v.bytes());
        REQUIRE(!p.channels.empty());
        for (const auto& c : p.channels) CHECK(c.decoded);
    }
}
