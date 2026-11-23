// Test kontraktow E0: kazdy zamrozony naglowek kompiluje sie samodzielnie (wlasciwa kolejnosc
// include sprawdza tez, ze naglowki sa samowystarczalne) + SpscRing.
// Brak zaleznosci od libvgm w naglowkach publicznych (test nie linkuje z niczym ponad options).
#include <doctest/doctest.h>

#include "vgmplay/i18n.hpp"
#include "vgmplay/input.hpp"
#include "vgmplay/library.hpp"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/decoder.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/format.hpp"
#include "vgmplay/layout.hpp"
#include "vgmplay/chips.hpp"
#include "vgmplay/core.hpp"

#include <thread>
#include <type_traits>

using namespace vgmplay;

TEST_CASE("naglowki kontraktow: domyslne wartosci zgodne z decyzjami")
{
    DecoderConfig dc;
    CHECK(dc.loopCount == 2);
    CHECK(dc.fadeMs == 1000);
    CHECK(dc.nonLoopEnd == NonLoopEnd::Stop);

    settings::Settings s;
    CHECK(s.schemaVersion == settings::kSettingsSchemaVersion);
    CHECK(s.playback.loopCount == 2);
    CHECK(s.playback.fadeMs == 1000);
    CHECK(s.playback.nonLoopEnd == NonLoopEnd::Stop);
    CHECK(s.ui.gd3Language == format::Gd3Language::En);
    CHECK(s.ui.language == i18n::Language::Pl);
    CHECK_FALSE(input::kGamepadWorksInBackground);

    library::ScanOptions so;
    CHECK_FALSE(so.followReparsePoints);
    CHECK(so.maxDepth == 32);
    CHECK(so.maxFiles == 100000);

    format::Limits lim;
    CHECK(lim.maxInputBytes == (64ull << 20));
}

TEST_CASE("Result i Error")
{
    Result<int> ok = 5;
    CHECK(ok.ok());
    CHECK(ok.value() == 5);
    Result<int> bad = makeError(ErrorCode::Truncated, "x", 12);
    CHECK_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Truncated);
    CHECK(bad.error().byteOffset == 12);
    Result<void> v;
    CHECK(v.ok());
    Result<void> ve = makeError(ErrorCode::Io, "io");
    CHECK_FALSE(ve.ok());
}

TEST_CASE("ScopeBlock jest POD")
{
    CHECK(std::is_trivially_copyable_v<ScopeBlock>);
    CHECK(sizeof(ScopeBlock) > 0);
}

TEST_CASE("SpscRing: push/pop, FIFO")
{
    SpscRing<int, 8> r;
    int v = -1;
    CHECK(r.empty());
    CHECK_FALSE(r.tryPop(v));
    for (int i = 0; i < 5; ++i) CHECK(r.tryPush(i));
    CHECK(r.sizeApprox() == 5);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(r.tryPop(v));
        CHECK(v == i);
    }
    CHECK(r.empty());
}

TEST_CASE("SpscRing: przepelnienie i zawijanie")
{
    SpscRing<int, 4> r;
    for (int i = 0; i < 4; ++i) CHECK(r.tryPush(i));
    CHECK_FALSE(r.tryPush(99));          // pelny - element odrzucony
    int v = 0;
    REQUIRE(r.tryPop(v));
    CHECK(v == 0);
    CHECK(r.tryPush(4));                 // zwolnione miejsce
    CHECK_FALSE(r.tryPush(5));
    for (int expect : {1, 2, 3, 4}) {    // brak elementu 99, kolejnosc zachowana
        REQUIRE(r.tryPop(v));
        CHECK(v == expect);
    }
    CHECK_FALSE(r.tryPop(v));
    // wielokrotne zawiniecie indeksow
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(r.tryPush(i));
        REQUIRE(r.tryPop(v));
        CHECK(v == i);
    }
}

TEST_CASE("SpscRing: typ nietrywialny (shared_ptr)")
{
    SpscRing<std::shared_ptr<int>, 2> r;
    auto p = std::make_shared<int>(7);
    CHECK(r.tryPush(p));
    CHECK(p.use_count() == 2);
    std::shared_ptr<int> out;
    CHECK(r.tryPop(out));
    CHECK(*out == 7);
}

TEST_CASE("SpscRing: producent i konsument na osobnych watkach")
{
    constexpr int kCount = 200000;
    SpscRing<int, 64> r;
    std::thread producer([&] {
        for (int i = 0; i < kCount;) {
            if (r.tryPush(i)) ++i;
        }
    });
    int expect = 0;
    bool ordered = true;
    while (expect < kCount) {
        int v;
        if (r.tryPop(v)) {
            if (v != expect) { ordered = false; break; }
            ++expect;
        }
    }
    if (!ordered) { // odblokuj producenta przy bledzie
        int v;
        while (r.tryPop(v)) {}
    }
    producer.join();
    CHECK(ordered);
    CHECK(expect == kCount);
}
