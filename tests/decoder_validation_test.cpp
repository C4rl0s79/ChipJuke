// Obrona w glebi dekodera: IDecoder::open() waliduje bufor SAM (format::parseVgmHeader + validateCommandStream),
// zanim cokolwiek trafi do libvgm. Wejscie omija loader, wiec plik uszkodzony nie moze dotrzec do libvgm.
#include <doctest/doctest.h>

#include <algorithm>
#include <set>

#include "test_util.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace {

using namespace vgmplay;

std::vector<std::uint8_t> readCorpus(const char* rel)
{
    return testutil::readAll(testutil::dataDir() / rel);
}

DecoderConfig cfg44()
{
    DecoderConfig c;
    c.sampleRate = 44100;
    return c;
}

} // namespace

TEST_CASE("dekoder: wszystkie pliki malformed/ i valid/ (.vgm) - open zgodny z format::*, bez crasha")
{
    std::size_t rejected = 0, accepted = 0;
    for (const char* sub : {"valid", "malformed"}) {
        for (const auto& e : std::filesystem::directory_iterator(testutil::dataDir() / sub)) {
            if (e.path().extension() != ".vgm") continue;
            const auto bytes = testutil::readAll(e.path());
            auto hdr = format::parseVgmHeader(bytes);
            bool expectOk = false;
            if (hdr) expectOk = format::validateCommandStream(bytes, hdr.value()).ok();
            auto dec = engine::makeVgmDecoder();
            auto r = dec->open(bytes, cfg44());
            INFO(e.path().filename().string());
            if (expectOk) {
                // Plik przyjety przez format moze zostac odrzucony przez libvgm/dekoder tylko z kodem DecoderFailure/BadHeader
                // (np. brak rdzenia) - tu oczekujemy jednak przyjecia dla calego korpusu.
                CHECK_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
                if (r.ok()) {
                    ++accepted;
                    // Dokrecenie: start + krotki render nie moze sie wysypac ani zawiesic.
                    if (dec->start().ok()) {
                        std::vector<float> buf(2u * kMaxBlockFrames);
                        for (int i = 0; i < 200; ++i)
                            if (dec->render(buf.data(), kMaxBlockFrames, nullptr).ended) break;
                    }
                }
            } else {
                CHECK_FALSE(r.ok());
                ++rejected;
            }
        }
    }
    MESSAGE("przyjete: " << accepted << ", odrzucone przez walidacje dekodera: " << rejected);
    CHECK(rejected > 20);
    CHECK(accepted > 20);
}

TEST_CASE("dekoder: blok PCM za punktem petli - bez petli, ostrzezenie BlockAfterLoop, tak jak w loaderze")
{
    // Surowe bajty (z petla w naglowku) - dekoder sam wylacza petle, jak loader.
    const auto bytes = readCorpus("malformed/blk_after_loop.vgm");
    REQUIRE_FALSE(bytes.empty());
    auto dec = engine::makeVgmDecoder();
    auto r = dec->open(bytes, cfg44());
    REQUIRE(r.ok());
    const auto& info = r.value();
    CHECK(info.loopFrames == 0);
    CHECK(info.introFrames == info.totalFrames);
    CHECK(std::any_of(info.warnings.begin(), info.warnings.end(),
                      [](const Warning& w) { return w.code == WarningCode::BlockAfterLoop; }));

    // Koniec utworu przy loopCount=2 nastepuje po jednym przejsciu (bez petli) + ciszy ogonowej.
    REQUIRE(dec->start().ok());
    std::vector<float> buf(2u * kMaxBlockFrames);
    std::uint64_t frames = 0;
    bool ended = false;
    for (int i = 0; i < 100000 && !ended; ++i) {
        const auto rr = dec->render(buf.data(), kMaxBlockFrames, nullptr);
        frames += rr.frames;
        ended = rr.ended;
    }
    REQUIRE(ended);
    const std::uint64_t onePass = info.totalFrames;
    CHECK(frames >= onePass);
    CHECK(frames <= onePass + 44100 /* cisza ogonowa 500 ms + zapas */);
}

TEST_CASE("dekoder: totalFrames i tagi pochodza z wlasnego skanu, nie z naglowka")
{
    const auto bytes = readCorpus("valid/sn76489_header_total_lies_v150.vgm");
    REQUIRE_FALSE(bytes.empty());
    auto hdr = format::parseVgmHeader(bytes);
    REQUIRE(hdr);
    auto scan = format::validateCommandStream(bytes, hdr.value());
    REQUIRE(scan);
    CHECK(hdr.value().totalSamples != scan.value().totalSamples);   // naglowek klamie

    auto dec = engine::makeVgmDecoder();
    auto r = dec->open(bytes, cfg44());
    REQUIRE(r.ok());
    CHECK(r.value().totalFrames == scan.value().totalSamples);   // 44100 Hz: ticki == klatki

    const auto gd3 = readCorpus("valid/sn76489_loop_gg_v110_gd3jp.vgm");
    auto dec2 = engine::makeVgmDecoder();
    auto r2 = dec2->open(gd3, cfg44());
    REQUIRE(r2.ok());
    CHECK(r2.value().tags.present);
    CHECK_FALSE(r2.value().tags.trackJp.empty());
}
