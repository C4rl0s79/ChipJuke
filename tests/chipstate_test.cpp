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

Pattern build(const std::vector<std::uint8_t>& bytes, chipstate::PatternOptions opt = {})
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
    auto p = chipstate::buildPattern(c.track.raw(), c.track.header, c.track.stream);
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
