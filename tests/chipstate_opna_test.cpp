// Testy dekoderow YM2608 (OPNA) i YM2610/YM2610B (OPNB) w chipstate: FM (F-number -> nuta z uwzglednieniem zegara
// i preskalera), SSG (AY), rytm YM2608, ADPCM-A (YM2610), ADPCM-B (delta-T). Syntetyczne strumienie VGM 1.71,
// wiersz = 735 tickow. Uklad kanalow: FM 1-6 (0-5), ADPCM-A 1-6 (6-11), ADPCM-B (12), AY 1-3 (13-15).
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "vgmplay/chipstate.hpp"
#include "vgmplay/format.hpp"

namespace {

using namespace vgmplay;
using chipstate::kNoteNone;
using chipstate::kNoteOff;
using chipstate::Pattern;

constexpr std::size_t kOpnaClockOfs = 0x48;    // YM2608
constexpr std::size_t kOpnbClockOfs = 0x4C;    // YM2610 (bit 31 zegara = YM2610B)
constexpr std::uint32_t kOpnaClock = 7'987'200;
constexpr std::uint32_t kOpnbClock = 8'000'000;

constexpr std::size_t kFm = 0, kAdpcmA = 6, kAdpcmB = 12, kSsg = 13;

class Stream {
public:
    Stream(std::size_t clockOfs, std::uint32_t clock, int cmd0, int cmd1) : hdr_(0x100, 0), cmd0_(cmd0), cmd1_(cmd1)
    {
        std::memcpy(hdr_.data(), "Vgm ", 4);
        put32(8, 0x171);
        put32(0x34, 0x100 - 0x34);
        put32(clockOfs, clock);
    }
    Stream& raw(std::initializer_list<int> b) { for (int v : b) d_.push_back(static_cast<std::uint8_t>(v)); return *this; }
    Stream& wait(std::uint32_t n)
    {
        total_ += n;
        while (n > 0) {
            const std::uint32_t c = n > 65535 ? 65535 : n;
            raw({0x61, static_cast<int>(c & 0xFF), static_cast<int>(c >> 8)});
            n -= c;
        }
        return *this;
    }
    Stream& p0(int r, int v) { return raw({cmd0_, r, v}); }
    Stream& p1(int r, int v) { return raw({cmd1_, r, v}); }
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
    int cmd0_, cmd1_;
    std::uint32_t total_ = 0;
};

Stream opna() { return Stream(kOpnaClockOfs, kOpnaClock, 0x56, 0x57); }
Stream opnb(bool typeB = false) { return Stream(kOpnbClockOfs, kOpnbClock | (typeB ? 0x80000000u : 0u), 0x58, 0x59); }

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

// FM kanal `lane` (0-2) na porcie `port`: ALG 7, TL 0 operatorow nosnych; F-number/blok.
void fmSetup(Stream& v, int port, int lane, int block, int fnum)
{
    auto w = [&](int r, int val) { port ? v.p1(r, val) : v.p0(r, val); };
    for (int slot = 0; slot < 4; ++slot) w(0x40 + slot * 4 + lane, 0);
    w(0xB0 + lane, 0x07);
    w(0xA4 + lane, (block << 3) | (fnum >> 8));
    w(0xA0 + lane, fnum & 0xFF);
}

// Nuta MIDI z czestotliwosci [Hz] (jak w dekoderze: zaokraglenie do najblizszego polotonu).
int midiOf(double hz) { return static_cast<int>(std::lround(69.0 + 12.0 * std::log2(hz / 440.0))); }

} // namespace

TEST_CASE("chipstate YM2608: uklad kanalow (16) i decoded; FM 1 i FM 4 (port 1) - F-number i zegar OPNA")
{
    // fnum 618, blok 4 przy zegarze 7987200 (FM rate = clock / 144 = 55466.7 Hz): 618 * 55466.7 * 2^-17 = 261.5 Hz = C-4.
    Stream v = opna();
    fmSetup(v, 0, 0, 4, 618);
    fmSetup(v, 1, 0, 4, 618);                // FM 4 (kanal 3)
    v.p0(0x28, 0xF0).p0(0x28, 0xF4);         // key-on FM 1 i FM 4
    v.wait(735);
    v.p0(0x28, 0x00).p0(0x28, 0x04);
    v.wait(735);
    const Pattern p = build(v.bytes());
    REQUIRE(p.channels.size() == 16);
    for (const auto& c : p.channels) CHECK(c.decoded);
    CHECK(p.channels[kFm].name == "FM 1");
    CHECK(p.channels[kAdpcmA].name == "ADPCM-A 1");
    CHECK(p.channels[kAdpcmB].name == "ADPCM-B");
    CHECK(p.channels[kSsg].name == "AY-3-8910 Ch 1");
    CHECK(p.at(0, 0).note == 60);
    CHECK(p.at(0, 3).note == 60);
    CHECK(p.at(0, 0).volume == 0x40);
    CHECK(p.at(1, 0).note == kNoteOff);
    CHECK(p.at(1, 3).note == kNoteOff);
    CHECK(p.at(0, 1).note == kNoteNone);
}

TEST_CASE("chipstate YM2608: preskaler FM (0x2F = /2 zamiast /6) podnosi wysokosc o 3x")
{
    Stream v = opna();
    v.p0(0x2F, 0);                           // sel = 0: dzielnik 48 zamiast 144
    fmSetup(v, 0, 0, 4, 618);
    v.p0(0x28, 0xF0);
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, 0).note == midiOf(618 * (kOpnaClock / 48.0) * std::ldexp(1.0, -17)));   // ~784.6 Hz
    CHECK(p.at(0, 0).note == 79);
}

TEST_CASE("chipstate YM2608: SSG - ton z okresu (zegar SSG = clock / 4), glosnosc i wylaczenie tonu")
{
    // TP 252: 7987200 / 4 / (16 * 252) = 495.2 Hz = B-4 (71).
    Stream v = opna();
    v.p0(0x00, 252).p0(0x01, 0).p0(0x07, 0x38).p0(0x08, 0x0F);
    v.wait(735);
    v.p0(0x08, 0x08);                        // amplituda 8 -> zmiana glosnosci bez nowej nuty
    v.wait(735);
    v.p0(0x08, 0x00);                        // cisza
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kSsg).note == 71);
    CHECK(p.at(0, kSsg).volume == 0x40);
    CHECK(p.at(1, kSsg).note == kNoteNone);
    CHECK(p.at(1, kSsg).volume == 34);
    CHECK(p.at(2, kSsg).note == kNoteOff);
    // Zapis na porcie 1 pod adresem 0x00-0x0F to nie SSG.
    Stream w = opna();
    w.p1(0x00, 252).p1(0x07, 0x38).p1(0x08, 0x0F);
    w.wait(735);
    const Pattern q = build(w.bytes());
    CHECK(q.at(0, kSsg).note == kNoteNone);
}

TEST_CASE("chipstate YM2608: rytm - key-on bitmaska, poziom, dump, koniec probki z tablicy ROM")
{
    Stream v = opna();
    v.p0(0x11, 0x3F);                        // total level: maks.
    v.p0(0x18, 0xDF).p0(0x19, 0xDF);         // bass, snare: poziom 31
    v.p0(0x10, 0x03);                        // bass + snare
    v.wait(735);
    v.p0(0x18, 0xD0);                        // obnizenie poziomu bassu w trakcie (atenuacja 15 -> 48/63)
    v.p0(0x10, 0x82);                        // dump snare
    v.wait(735 * 8);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kAdpcmA + 0).note == 60);
    CHECK(p.at(0, kAdpcmA + 0).instrument == 0);
    CHECK(p.at(0, kAdpcmA + 0).volume == 0x40);
    CHECK(p.at(0, kAdpcmA + 1).note == 60);
    CHECK(p.at(0, kAdpcmA + 1).instrument == 1);
    CHECK(p.at(1, kAdpcmA + 1).note == kNoteOff);   // dump
    CHECK(p.at(1, kAdpcmA + 0).volume == 49);       // scaleVolume(48, 63)
    // Bass drum: 447 bajtow = 894 nibbli przy 7987200 / 144 / 3 = 18489 nibbli/s => ok. 2133 tickow => wiersz 2.
    bool off = false;
    for (std::uint64_t r = 1; r < 5; ++r) off = off || p.at(r, kAdpcmA + 0).note == kNoteOff;
    CHECK(off);
    CHECK(p.at(0, kAdpcmA + 2).note == kNoteNone);
}

TEST_CASE("chipstate YM2608: ADPCM-B - start/stop, nuta wzgledna z delta-N, glosnosc, koniec probki")
{
    Stream v = opna();
    v.p1(0x01, 0xC1);                        // L+R, ROM (przesuniecie adresow 5)
    v.p1(0x02, 0x00).p1(0x03, 0x00).p1(0x04, 0x40).p1(0x05, 0x00);   // start 0, koniec 0x40
    v.p1(0x09, 0x00).p1(0x0A, 0x80);         // delta-N 0x8000 = polowa tempa => C-3 (48)
    v.p1(0x0B, 0xFF);
    v.p1(0x00, 0xA0);                        // START + pamiec zewnetrzna
    v.wait(735);
    v.p1(0x0B, 0x80);                        // glosnosc w trakcie
    v.wait(735 * 12);
    v.p1(0x00, 0xA0);                        // ponowny start (ta sama probka, ten sam instrument)
    v.wait(735);
    v.p1(0x00, 0x01);                        // reset = stop
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kAdpcmB).note == 48);
    CHECK(p.at(0, kAdpcmB).instrument == 0);
    CHECK(p.at(0, kAdpcmB).volume == 0x40);
    CHECK(p.at(1, kAdpcmB).volume == 0x20);
    // (0x40 << 5 | 31) + 1 = 2080 bajtow = 4160 nibbli przy 55466.7 * 0.5 nibbli/s => 0.15 s = wiersz 9.
    CHECK(p.at(9, kAdpcmB).note == kNoteOff);
    CHECK(p.at(13, kAdpcmB).note == 48);
    CHECK(p.at(13, kAdpcmB).instrument == 0);
    CHECK(p.at(14, kAdpcmB).note == kNoteOff);
}

TEST_CASE("chipstate YM2610: FM - kanaly 1 i 4 nieuzywane (bez B), YM2610B ma wszystkie 6; zegar / 144")
{
    auto make = [](bool typeB) {
        Stream v = opnb(typeB);
        fmSetup(v, 0, 1, 4, 618);            // FM 2
        fmSetup(v, 0, 0, 4, 618);            // FM 1
        fmSetup(v, 1, 1, 4, 618);            // FM 5
        v.p0(0x28, 0xF1).p0(0x28, 0xF0).p0(0x28, 0xF5);
        v.wait(735);
        return build(v.bytes());
    };
    const Pattern a = make(false);
    REQUIRE(a.channels.size() == 16);
    for (const auto& c : a.channels) CHECK(c.decoded);
    // 618 * (8000000 / 144) * 2^-17 = 261.9 Hz = C-4.
    CHECK(a.at(0, 1).note == 60);
    CHECK(a.at(0, 4).note == 60);
    CHECK(a.at(0, 0).note == kNoteNone);
    const Pattern b = make(true);
    CHECK(b.at(0, 0).note == 60);
    CHECK(b.at(0, 1).note == 60);
}

TEST_CASE("chipstate YM2610: SSG (clock / 4) - ton z okresu")
{
    Stream v = opnb();
    v.p0(0x02, 252).p0(0x03, 0).p0(0x07, 0x3D).p0(0x09, 0x0F);   // kanal B (AY 2): 8000000/4/(16*252) = 496 Hz
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kSsg + 1).note == 71);
    CHECK(p.at(0, kSsg).note == kNoteNone);
}

TEST_CASE("chipstate YM2610: ADPCM-A - key-on bitmaska z adresami start/koniec, TL i poziom kanalu, dump")
{
    Stream v = opnb();
    v.p1(0x01, 0x3F);                        // total level
    v.p1(0x08, 0xDF).p1(0x09, 0xDF);         // poziomy kanalow 1 i 2
    v.p1(0x10, 0x00).p1(0x18, 0x00).p1(0x20, 0x0F).p1(0x28, 0x00);   // kanal 1: probka 0x000000-0x000FFF
    v.p1(0x11, 0x20).p1(0x19, 0x00).p1(0x21, 0x3F).p1(0x29, 0x00);   // kanal 2: inna probka
    v.p1(0x00, 0x03);                        // key-on 1 i 2
    v.wait(735);
    v.p1(0x01, 0x20);                        // total level nizej: atenuacja 31 -> (63-31)/63
    v.p1(0x00, 0x81);                        // dump kanalu 1
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kAdpcmA + 0).note == 60);
    CHECK(p.at(0, kAdpcmA + 0).instrument == 0);
    CHECK(p.at(0, kAdpcmA + 0).volume == 0x40);
    CHECK(p.at(0, kAdpcmA + 1).note == 60);
    CHECK(p.at(0, kAdpcmA + 1).instrument == 1);   // inna probka = inny instrument
    CHECK(p.at(1, kAdpcmA + 0).note == kNoteOff);
    CHECK(p.at(1, kAdpcmA + 1).volume == 33);      // scaleVolume(32, 63)
}

TEST_CASE("chipstate YM2610: ADPCM-B - rejestry 0x10-0x1B na porcie 0, start/stop")
{
    Stream v = opnb();
    v.p0(0x11, 0xC0);
    v.p0(0x12, 0x00).p0(0x13, 0x00).p0(0x14, 0xFF).p0(0x15, 0x0F);
    v.p0(0x19, 0x55).p0(0x1A, 0x55);         // delta-N 0x5555 = 1/3 => 60 + 12*log2(1/3) = 41
    v.p0(0x1B, 0xC0);
    v.p0(0x10, 0x90);                        // START + REPEAT
    v.wait(735 * 3);
    v.p0(0x10, 0x01);                        // reset
    v.wait(735);
    const Pattern p = build(v.bytes());
    CHECK(p.at(0, kAdpcmB).note == 41);
    CHECK(p.at(0, kAdpcmB).volume == 48);
    CHECK(p.at(1, kAdpcmB).note == kNoteNone);   // z REPEAT trwa do stopu
    CHECK(p.at(2, kAdpcmB).note == kNoteNone);
    CHECK(p.at(3, kAdpcmB).note == kNoteOff);
}
