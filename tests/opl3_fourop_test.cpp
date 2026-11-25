// OPL3 (YMF262) 4-op: para kanalow brzmi jako jeden kanal. Nuta jest na pierwszej polowce (1..3, 10..12),
// a rdzenie wyprowadzaja dzwiek pary przez druga (4..6, 13..15). MUTE/SOLO i oscyloskop pierwszej polowki
// musza obejmowac cala pare, druga polowka jest cicha. Zgloszenie: Dark Seraphim BGM 02 (0x104 = 0x2F).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "vgmplay/format.hpp"
#include "vgmplay/vgm_decoder.hpp"

using namespace vgmplay;
using namespace vgmplay::engine;

namespace {

constexpr std::uint32_t kOpl3Clock = 14318180;

void le32(std::vector<std::uint8_t>& v, std::size_t pos, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[pos + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

// Para 1+4 w trybie 4-op (0x104 bit 0), wszystkie cztery operatory glosne, key-on na kanale 1.
std::vector<std::uint8_t> fourOpTrack(bool fourOp)
{
    std::vector<std::uint8_t> f(0x80, 0);
    f[0] = 'V'; f[1] = 'g'; f[2] = 'm'; f[3] = ' ';
    le32(f, 0x08, 0x151);
    le32(f, 0x34, 0x80 - 0x34);
    le32(f, 0x5C, kOpl3Clock);
    auto w = [&](int port, std::uint8_t r, std::uint8_t v) {
        f.push_back(port ? 0x5F : 0x5E); f.push_back(r); f.push_back(v);
    };
    w(1, 0x05, 0x01);                        // tryb OPL3
    w(1, 0x04, fourOp ? 0x01 : 0x00);
    for (int off : {0x00, 0x03, 0x08, 0x0B}) {   // operatory kanalow 1 i 4
        w(0, static_cast<std::uint8_t>(0x20 + off), 0x01);
        w(0, static_cast<std::uint8_t>(0x40 + off), 0x00);
        w(0, static_cast<std::uint8_t>(0x60 + off), 0xF0);
        w(0, static_cast<std::uint8_t>(0x80 + off), 0x0F);
    }
    w(0, 0xC0, 0x31);
    w(0, 0xC3, 0x31);
    w(0, 0xA0, 0x44);
    w(0, 0xB0, 0x31);                        // key-on, blok 4
    const std::uint32_t ticks = 44100;
    f.push_back(0x61); f.push_back(0x44); f.push_back(0xAC);
    f.push_back(0x66);
    le32(f, 0x18, ticks);
    le32(f, 0x04, static_cast<std::uint32_t>(f.size() - 4));
    return f;
}

// RMS miksu przy masce wyciszenia ukladu (bit = kanal wyciszony).
double rmsWithMute(const std::vector<std::uint8_t>& track, std::uint32_t muteMain)
{
    auto dec = makeVgmDecoder();
    DecoderConfig cfg;
    cfg.sampleRate = 44100;
    auto info = dec->open(track, cfg);
    REQUIRE(info.ok());
    REQUIRE(dec->start().ok());
    dec->setMute(0, MuteMask{muteMain, 0xFFFFFFFFu});
    std::vector<float> buf(2 * 1024);
    double sum = 0;
    std::size_t n = 0;
    for (int b = 0; b < 16; ++b) {
        const RenderResult r = dec->render(buf.data(), 1024, nullptr);
        for (std::uint32_t i = 0; i < 2 * r.frames; ++i) { sum += double(buf[i]) * buf[i]; ++n; }
    }
    return n ? std::sqrt(sum / double(n)) : 0.0;
}

} // namespace

TEST_SUITE("opl3_fourop")
{
    TEST_CASE("pre-skan zbiera pary 4-op z rejestru 0x104")
    {
        const auto t = fourOpTrack(true);
        auto h = format::parseVgmHeader(t);
        REQUIRE(h.ok());
        auto s = format::validateCommandStream(t, h.value());
        REQUIRE(s.ok());
        CHECK(s.value().opl3FourOp[0] == 0x01);
        CHECK(s.value().opl3FourOp[1] == 0x00);
    }

    TEST_CASE("solo pierwszej polowki pary 4-op daje dzwiek, solo drugiej - cisze")
    {
        const auto t = fourOpTrack(true);
        const std::uint32_t all = 0xFFFFFFFFu;
        const double full = rmsWithMute(t, 0);
        const double soloFirst = rmsWithMute(t, all & ~1u);
        const double soloSecond = rmsWithMute(t, all & ~(1u << 3));
        const double muteFirst = rmsWithMute(t, 1u);
        REQUIRE(full > 0.0005);
        CHECK(soloFirst == doctest::Approx(full).epsilon(0.01));
        CHECK(soloSecond < 1e-5);
        CHECK(muteFirst < 1e-5);
    }

    TEST_CASE("bez 4-op kanaly 1 i 4 pozostaja niezalezne")
    {
        const auto t = fourOpTrack(false);
        CHECK(rmsWithMute(t, 0xFFFFFFFFu & ~1u) > 0.0005);   // nuta tylko na kanale 1 (2-op: ciszej)
        CHECK(rmsWithMute(t, 1u) < 1e-5);
    }
}
