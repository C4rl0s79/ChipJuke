// Blok PCM w punkcie petli lub za nim: plik jest przyjmowany, gra raz, bez petli
// (decyzja uzytkownika 2026-11-23). Dane: korpus tests/data (tools/vgmgen).
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

#include "vgmplay/loader.hpp"

namespace {

std::uint32_t rd32(const vgmplay::ByteBuffer& b, std::size_t ofs)
{
    return static_cast<std::uint32_t>(b[ofs]) | (static_cast<std::uint32_t>(b[ofs + 1]) << 8) |
           (static_cast<std::uint32_t>(b[ofs + 2]) << 16) | (static_cast<std::uint32_t>(b[ofs + 3]) << 24);
}

} // namespace

TEST_CASE("blok PCM za punktem petli: plik gra raz, petla wyzerowana w buforze")
{
    const std::filesystem::path p = std::filesystem::path(VGMPLAY_TEST_DATA_DIR) / "malformed" / "blk_after_loop.vgm";
    REQUIRE(std::filesystem::exists(p));

    auto r = vgmplay::loader::loadTrackFromFile(p);
    REQUIRE(r);
    const auto& t = r.value();

    CHECK(t.stream.loopDisabled);
    CHECK_FALSE(t.header.hasLoop());
    CHECK(std::any_of(t.warnings.begin(), t.warnings.end(),
                      [](const vgmplay::Warning& w) { return w.code == vgmplay::WarningCode::BlockAfterLoop; }));
    REQUIRE(t.bytes);
    CHECK(rd32(*t.bytes, 0x1C) == 0u);   // loop offset
    CHECK(rd32(*t.bytes, 0x20) == 0u);   // loop samples
}

TEST_CASE("poprawny plik z petla zachowuje petle")
{
    const std::filesystem::path p =
        std::filesystem::path(VGMPLAY_TEST_DATA_DIR) / "valid" / "ym2612_melody_loop_v150_gd3jp.vgm";
    REQUIRE(std::filesystem::exists(p));

    auto r = vgmplay::loader::loadTrackFromFile(p);
    REQUIRE(r);
    CHECK_FALSE(r.value().stream.loopDisabled);
    CHECK(r.value().header.hasLoop());
}
