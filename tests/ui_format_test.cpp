// Testy czystej logiki UI: formatowanie czasu/zegarow, pasek pozycji, ScopeBuffer, tlumaczenia.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vgmplay_ui/format.h"
#include "vgmplay_ui/scope_buffer.h"
#include "vgmplay_ui/strings.h"

using namespace vgmplay;
using namespace vgmplay::ui;

TEST_SUITE("ui/format")
{
    TEST_CASE("formatTime")
    {
        CHECK(formatTime(0) == "0:00");
        CHECK(formatTime(-3) == "0:00");
        CHECK(formatTime(std::nan("")) == "0:00");
        CHECK(formatTime(65.9) == "1:05");
        CHECK(formatTime(3599) == "59:59");
        CHECK(formatTime(3600) == "1:00:00");
    }

    TEST_CASE("sanitizeForDisplay usuwa znaki kierunkowe Unicode")
    {
        CHECK(sanitizeForDisplay("ab\xE2\x80\xAE" "cd") == "abcd");   // U+202E
        CHECK(sanitizeForDisplay("\xE2\x80\x8B" "x\xE2\x81\xA6" "y\xE2\x81\xA9") == "xy");
        CHECK(sanitizeForDisplay("\xE3\x82\xB0 \xE2\x80\x93") == "\xE3\x82\xB0 \xE2\x80\x93"); // zwykle znaki zostaja
        CHECK(sanitizeForDisplay("\xE2\x80") == "\xE2\x80");
    }

    TEST_CASE("formatClock")
    {
        CHECK(formatClock(3579545) == "3.580 MHz");
        CHECK(formatClock(985248) == "985 kHz");
        CHECK(formatClock(440) == "440 Hz");
    }

    TEST_CASE("pasek pozycji: znana dlugosc i petla nieskonczona")
    {
        SeekBar a = makeSeekBar(500, 1000, 100, 200, 300);
        CHECK(a.length == 1000);
        CHECK(a.position == 500);
        CHECK_FALSE(a.infinite);
        CHECK(makeSeekBar(5000, 1000, 100, 200, 300).position == 1000);

        SeekBar b = makeSeekBar(100 + 200 * 3 + 50, 0, 100, 200, 300);
        CHECK(b.infinite);
        CHECK(b.length == 300);
        CHECK(b.position == 150);                       // zawiniete w petli
        CHECK(makeSeekBar(50, 0, 100, 200, 300).position == 50);
        CHECK(makeSeekBar(10, 0, 0, 0, 0).length == 0);
    }

    TEST_CASE("seek: ulamek i przesuniecie sa obciete do zakresu")
    {
        CHECK(fractionToFrame(0.0, 1000) == 0);
        CHECK(fractionToFrame(0.5, 1000) == 500);
        CHECK(fractionToFrame(1.0, 1000) == 999);
        CHECK(fractionToFrame(7.0, 1000) == 999);
        CHECK(fractionToFrame(-1.0, 1000) == 0);
        CHECK(fractionToFrame(0.5, 0) == 0);

        CHECK(offsetFrame(48000 * 10, 5.0, 48000, 48000 * 100) == 48000 * 15);
        CHECK(offsetFrame(48000 * 2, -5.0, 48000, 48000 * 100) == 0);
        CHECK(offsetFrame(48000 * 98, 5.0, 48000, 48000 * 100) == 48000 * 100 - 1);
        CHECK(offsetFrame(10, 5.0, 48000, 0) == 0);
    }
}

namespace {
void fill(ScopeBlock& b, std::uint64_t epoch, std::uint64_t start, std::uint32_t frames, double period)
{
    b.epoch = epoch;
    b.startFrame = start;
    b.frames = frames;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(std::sin(2.0 * 3.14159265358979 * static_cast<double>(start + i) / period));
        b.masterL[i] = v;
        b.masterR[i] = 0.5f * v;
    }
}
} // namespace

TEST_SUITE("ui/scope")
{
    TEST_CASE("bufor scope: kolejne bloki, trigger na narastajacym zerze")
    {
        auto buf = std::make_unique<ScopeBuffer>();
        auto blk = std::make_unique<ScopeBlock>();
        for (int i = 0; i < 8; ++i) {
            fill(*blk, 1, 512u * static_cast<unsigned>(i), 512, 100.0);
            buf->push(*blk);
        }
        CHECK(buf->epoch() == 1);
        CHECK(buf->endFrame() == 4096);
        std::vector<float> l, r;
        std::uint64_t at = 0;
        REQUIRE(buf->extract(1000, 1024, l, r, &at));
        CHECK(at >= 1000);
        CHECK(at < 1000 + 1024);
        CHECK(l.size() == 1024);
        // Poczatek okna: sinus przechodzi z ujemnej na nieujemna wartosc.
        CHECK(l[0] >= 0.0f);
        CHECK(l[0] < 0.2f);
        CHECK(r[0] == doctest::Approx(0.5f * l[0]));
        // Okno nie wychodzi poza dane, gdy pozycja slyszana jest przy koncu.
        REQUIRE(buf->extract(4090, 1024, l, r, &at));
        CHECK(at + 1024 <= buf->endFrame());
    }

    TEST_CASE("bufor scope: nowa epoka zeruje, stara jest odrzucana, luka zeruje")
    {
        auto buf = std::make_unique<ScopeBuffer>();
        auto blk = std::make_unique<ScopeBlock>();
        fill(*blk, 2, 0, 512, 100.0);
        buf->push(*blk);
        fill(*blk, 1, 512, 512, 100.0); // minela epoka
        buf->push(*blk);
        CHECK(buf->epoch() == 2);
        CHECK(buf->endFrame() == 512);
        fill(*blk, 3, 48000, 512, 100.0); // seek: nowa epoka, inna pozycja
        buf->push(*blk);
        CHECK(buf->epoch() == 3);
        CHECK(buf->beginFrame() == 48000);
        CHECK(buf->endFrame() == 48512);
        fill(*blk, 3, 60000, 512, 100.0); // luka w tej samej epoce
        buf->push(*blk);
        CHECK(buf->beginFrame() == 60000);
        std::vector<float> l, r;
        CHECK_FALSE(buf->extract(60000, 1024, l, r)); // za malo danych
    }
}

TEST_SUITE("ui/strings")
{
    TEST_CASE("kazdy kod bledu i ostrzezenia ma tekst PL i EN oraz nazwe")
    {
        for (const Lang lg : {Lang::En, Lang::Pl}) {
            setLang(lg);
            for (std::uint16_t c = static_cast<std::uint16_t>(ErrorCode::InvalidArgument);
                 c <= static_cast<std::uint16_t>(ErrorCode::Internal); ++c) {
                const auto code = static_cast<ErrorCode>(c);
                CHECK(std::string(trError(code)).size() > 3);
                CHECK(std::string(errorCodeName(code)) != "Unknown");
            }
            for (std::uint16_t c = static_cast<std::uint16_t>(WarningCode::UnknownVersion);
                 c <= static_cast<std::uint16_t>(WarningCode::Other); ++c) {
                const auto code = static_cast<WarningCode>(c);
                CHECK(std::string(trWarning(code)).size() > 3);
                CHECK(std::string(warningCodeName(code)) != "Unknown");
            }
            for (int s = 0; s < static_cast<int>(Str::Count); ++s)
                CHECK(std::string(tr(static_cast<Str>(s))).size() > 0);
        }
        setLang(Lang::En);
        CHECK(std::string(trWarning(WarningCode::BlockAfterLoop)).find("loop disabled") != std::string::npos);
        setLang(Lang::Pl);
        CHECK(std::string(trWarning(WarningCode::BlockAfterLoop)).find("pętla wyłączona") != std::string::npos);
        setLang(Lang::En);
    }
}
