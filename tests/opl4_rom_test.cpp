// OPL4 (YMF278B): libvgm prosi o zewnetrzny ROM probek yrw801.rom. Dekoder szuka go w DecoderConfig::romDirs
// (katalog ChipJuke.exe, katalog utworu) i ostrzega (RomMissing), gdy go nie ma. ROM-u nie dolaczamy.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "vgmplay/vgm_decoder.hpp"

using namespace vgmplay;
using namespace vgmplay::engine;

namespace {

std::vector<std::uint8_t> opl4Track()
{
    std::vector<std::uint8_t> f(0x80, 0);
    auto le32 = [&](std::size_t pos, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) f[pos + i] = static_cast<std::uint8_t>(x >> (8 * i));
    };
    f[0] = 'V'; f[1] = 'g'; f[2] = 'm'; f[3] = ' ';
    le32(0x08, 0x151);
    le32(0x34, 0x80 - 0x34);
    le32(0x60, 33868800);                     // YMF278B
    f.push_back(0x61); f.push_back(0x44); f.push_back(0xAC);   // 1 s ciszy
    f.push_back(0x66);
    le32(0x18, 44100);
    le32(0x04, static_cast<std::uint32_t>(f.size() - 4));
    return f;
}

bool hasRomWarning(const TrackInfo& ti)
{
    return std::any_of(ti.warnings.begin(), ti.warnings.end(),
                       [](const Warning& w) { return w.code == WarningCode::RomMissing; });
}

} // namespace

TEST_SUITE("opl4_rom")
{
    TEST_CASE("brak yrw801.rom w katalogach -> ostrzezenie; plik w katalogu -> bez ostrzezenia, start dziala")
    {
        const auto t = opl4Track();
        const auto dir = std::filesystem::temp_directory_path() / "vgmplay_opl4_rom_test";
        std::filesystem::create_directories(dir);
        std::filesystem::remove(dir / "yrw801.rom");

        DecoderConfig cfg;
        cfg.sampleRate = 44100;
        cfg.romDirs = {dir};
        {
            auto dec = makeVgmDecoder();
            auto info = dec->open(t, cfg);
            REQUIRE(info.ok());
            CHECK(hasRomWarning(info.value()));
            CHECK(dec->start().ok());
        }
        {
            std::ofstream(dir / "yrw801.rom", std::ios::binary) << std::string(0x1000, '\0');   // atrapa (nie prawdziwy ROM)
            auto dec = makeVgmDecoder();
            auto info = dec->open(t, cfg);
            REQUIRE(info.ok());
            CHECK_FALSE(hasRomWarning(info.value()));
            CHECK(dec->start().ok());
            std::vector<float> buf(2 * 512);
            CHECK(dec->render(buf.data(), 512, nullptr).frames > 0);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}
