// Testy modulu audio_out: PcmRing (epoka, declick, underrun, glosnosc, pozycja slyszana),
// brak alokacji w sciezce callbacku oraz AudioOut na backendzie "null" miniaudio.
// Test prawdziwego urzadzenia (WASAPI) tylko z VGMPLAY_TEST_AUDIO=1 (pomiar opoznienia).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#include "vgmplay/audio_out.hpp"
#include "vgmplay/pcm_ring.hpp"

// ---------------------------------------------------------------------------------------------
// Licznik alokacji: zastepuje globalne operator new w calym vgmplay_tests (liczy tylko, gdy
// biezacy watek wlaczyl licznik). Pozwala udowodnic brak alokacji w PcmRing::consume().
// ---------------------------------------------------------------------------------------------
namespace {
thread_local bool g_countAllocs = false;
thread_local std::size_t g_allocs = 0;
void* countedAlloc(std::size_t n)
{
    if (g_countAllocs) ++g_allocs;
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* countedAlignedAlloc(std::size_t n, std::size_t al)
{
    if (g_countAllocs) ++g_allocs;
    if (n == 0) n = 1;
    if (void* p = _aligned_malloc(n, al)) return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    try { return countedAlloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    try { return countedAlloc(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t al) { return countedAlignedAlloc(n, static_cast<std::size_t>(al)); }
void* operator new[](std::size_t n, std::align_val_t al) { return countedAlignedAlloc(n, static_cast<std::size_t>(al)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }

using namespace vgmplay;

namespace {

// Publikuje blok o stalej wartosci (L=v, R=-v).
bool publishConst(PcmRing& ring, std::uint64_t epoch, std::uint64_t start, std::uint32_t frames, float v)
{
    PcmRing::Block* b = ring.acquireWrite();
    if (!b) return false;
    b->epoch = epoch;
    b->startFrame = start;
    b->frames = frames;
    for (std::uint32_t i = 0; i < frames; ++i) { b->samples[2 * i] = v; b->samples[2 * i + 1] = -v; }
    ring.publish();
    return true;
}

} // namespace

TEST_SUITE("audio_out/pcm_ring")
{
    TEST_CASE("dane przechodza 1:1 po narastaniu; pozycja slyszana z klatek zuzytych")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        REQUIRE(publishConst(ring, 1, 1000, 512, 0.5f));
        REQUIRE(publishConst(ring, 1, 1512, 512, 0.5f));
        std::vector<float> out(2 * 600);
        ring.consume(out.data(), 600);
        // po narastaniu (96 klatek) pelna wartosc
        CHECK(out[2 * 100] == doctest::Approx(0.5f));
        CHECK(out[2 * 100 + 1] == doctest::Approx(-0.5f));
        CHECK(out[0] == doctest::Approx(0.0f));                  // zaczyna od zera (bez klikniecia)
        CHECK(out[2 * 50] < 0.5f);
        const auto a = ring.audible();
        REQUIRE(a.valid);
        CHECK(a.frame == 1000 + 600);
        CHECK(a.epoch24 == 1);
        CHECK(ring.queuedFrames() == 1024 - 600);
        CHECK(ring.consumedFrames() == 600);
    }

    TEST_CASE("zmiana epoki: stare bloki odrzucone, grajaca tresc zanika, potem cisza")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        for (int i = 0; i < 4; ++i) REQUIRE(publishConst(ring, 1, i * 512u, 512, 0.5f));
        std::vector<float> out(2 * 512);
        ring.consume(out.data(), 512);                           // gra
        ring.beginEpoch(2);                                      // seek
        ring.consume(out.data(), 512);
        // zanik od ostatniej wartosci do zera w kDeclickFrames klatek, dalej cisza
        CHECK(out[0] > 0.3f);
        for (std::uint32_t i = 1; i < PcmRing::kDeclickFrames; ++i)
            CHECK(std::fabs(out[2 * i]) <= std::fabs(out[2 * (i - 1)]) + 1e-6f);
        for (std::uint32_t i = PcmRing::kDeclickFrames; i < 512; ++i) CHECK(out[2 * i] == 0.0f);
        CHECK(ring.queuedFrames() == 0);                         // nic nie zostalo ze starej epoki
        // nowa epoka gra od nowej pozycji
        REQUIRE(publishConst(ring, 2, 7000, 512, 0.25f));
        ring.consume(out.data(), 300);
        CHECK(out[2 * 200] == doctest::Approx(0.25f));
        CHECK(ring.audible().frame == 7300);
        CHECK(ring.audible().epoch24 == 2);
    }

    TEST_CASE("underrun: liczony tylko gdy silnik jest aktywny, jeden na callback; zanik zamiast skoku")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        std::vector<float> out(2 * 256);
        ring.consume(out.data(), 256);
        CHECK(ring.underruns() == 0);                            // nieaktywny: pauza/stop
        ring.setActive(true);
        ring.consume(out.data(), 256);
        ring.consume(out.data(), 256);
        CHECK(ring.underruns() == 2);
        for (float f : out) CHECK(f == 0.0f);

        REQUIRE(publishConst(ring, 1, 0, 128, 0.8f));
        ring.consume(out.data(), 256);                           // 128 danych + brak => zanik, bez skoku
        CHECK(out[2 * 127] > 0.5f);
        CHECK(out[2 * 128] > 0.5f * 0.8f * 0.9f);                // zanik zaczyna sie od ostatniej wartosci
        CHECK(out[2 * 255] == doctest::Approx(0.0f).epsilon(0.1));
        CHECK(ring.underruns() == 3);                            // po zaniku (96 klatek) pusty pierscien = underrun
    }

    TEST_CASE("glosnosc: lagodne przejscie i skalowanie")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        for (int i = 0; i < 6; ++i) REQUIRE(publishConst(ring, 1, i * 512u, 512, 1.0f));
        std::vector<float> out(2 * 512);
        ring.consume(out.data(), 512);                           // narastanie po starcie
        ring.setVolume(0.5f);
        ring.consume(out.data(), 512);                           // przejscie 1.0 -> 0.5
        CHECK(out[0] > 0.9f);
        CHECK(out[2 * 511] == doctest::Approx(0.5f).epsilon(0.02));
        for (std::uint32_t i = 1; i < 512; ++i) CHECK(out[2 * i] <= out[2 * (i - 1)] + 1e-6f);
        ring.consume(out.data(), 512);
        CHECK(out[2 * 100] == doctest::Approx(0.5f));
        ring.setVolume(0.0f);
        ring.consume(out.data(), 512);
        ring.consume(out.data(), 512);
        CHECK(out[2 * 10] == 0.0f);
    }

    TEST_CASE("pelny pierscien: acquireWrite zwraca nullptr, potem znow miejsce")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        std::size_t n = 0;
        while (publishConst(ring, 1, n * 512u, 512, 0.1f)) ++n;
        CHECK(n == PcmRing::kBlocks);
        std::vector<float> out(2 * 512);
        ring.consume(out.data(), 512);
        CHECK(publishConst(ring, 1, 99999, 512, 0.1f));
    }

    TEST_CASE("brak alokacji w consume() (sciezka callbacku)")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        ring.setActive(true);
        std::vector<float> out(2 * 480);
        // przygotowanie poza pomiarem
        for (int i = 0; i < 8; ++i) REQUIRE(publishConst(ring, 1, i * 512u, 512, 0.3f));
        g_allocs = 0;
        g_countAllocs = true;
        for (int i = 0; i < 20; ++i) ring.consume(out.data(), 480);     // dane, underrun
        ring.beginEpoch(2);
        for (int i = 0; i < 5; ++i) ring.consume(out.data(), 480);      // zmiana epoki, cisza
        ring.setVolume(0.2f);
        ring.consume(out.data(), 480);
        g_countAllocs = false;
        CHECK(g_allocs == 0);

        // sanity: licznik w ogole dziala
        g_allocs = 0;
        g_countAllocs = true;
        std::vector<int> v(10);
        g_countAllocs = false;
        CHECK(g_allocs >= 1);
    }

    TEST_CASE("producent i konsument w osobnych watkach: bez utraty i duplikacji klatek")
    {
        PcmRing ring;
        ring.beginEpoch(1);
        constexpr std::uint32_t kBlocksToSend = 3000;
        std::atomic<bool> done{false};
        std::thread prod([&] {
            std::uint64_t start = 0;
            for (std::uint32_t i = 0; i < kBlocksToSend;) {
                if (publishConst(ring, 1, start, 256, 0.5f)) { start += 256; ++i; }
                else std::this_thread::yield();
            }
            done.store(true);
        });
        std::vector<float> out(2 * 192);
        double maxAbs = 0;
        std::uint64_t lastAudible = 0;
        bool monotonic = true;
        while (!done.load() || ring.queuedFrames() > 0) {
            ring.consume(out.data(), 192);
            for (float f : out) maxAbs = std::max(maxAbs, static_cast<double>(std::fabs(f)));
            const auto a = ring.audible();
            if (a.valid) {
                if (a.frame < lastAudible) monotonic = false;
                lastAudible = a.frame;
            }
        }
        prod.join();
        CHECK(ring.consumedFrames() == 256ull * kBlocksToSend);
        CHECK(lastAudible == 256ull * kBlocksToSend);
        CHECK(monotonic);
        CHECK(maxAbs <= 0.5 + 1e-6);
    }
}

TEST_SUITE("audio_out/device")
{
    TEST_CASE("AudioOut na backendzie null: open/start/stop, callback czyta z PcmRing")
    {
        PcmRing ring;
        audio_out::AudioOut out(ring);
        audio_out::Config cfg;
        cfg.backend = audio_out::Config::Backend::Null;
        cfg.sampleRate = 48000;
        auto r = out.open(cfg);
        REQUIRE(r.ok());
        CHECK(out.isOpen());
        CHECK(out.sampleRate() == 48000);
        CHECK(r.value().channels == 2);
        CHECK(r.value().periodFrames > 0);
        REQUIRE(out.start().ok());
        CHECK(out.isStarted());

        ring.beginEpoch(1);
        ring.setActive(true);
        std::uint64_t start = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300)) {
            if (publishConst(ring, 1, start, 512, 0.1f)) start += 512;
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        out.stop();
        CHECK_FALSE(out.isStarted());
        CHECK(ring.consumedFrames() > 4800);                     // callback pracowal
        CHECK(ring.audible().valid);
        const auto ev = out.pollEvents();
        CHECK_FALSE(ev.lost);
        out.close();
        CHECK_FALSE(out.isOpen());

        // reopen po zamknieciu dziala i ponownie otwiera urzadzenie
        REQUIRE(out.open(cfg).ok());
        auto rr = out.reopen();
        REQUIRE(rr.ok());
        CHECK(out.isOpen());
    }

    TEST_CASE("listDevices: backend null = jedna pozycja stala")
    {
        auto r = audio_out::listDevices(audio_out::Config::Backend::Null);
        REQUIRE(r.ok());
        REQUIRE(r.value().size() == 1);
        CHECK(r.value()[0].id == "null");
        CHECK(r.value()[0].name == "Null");
        CHECK(r.value()[0].isDefault);
    }

    TEST_CASE("listDevices: backend domyslny nie zwraca bledu (lub jest pomijany bez urzadzen)")
    {
        auto r = audio_out::listDevices();
        if (!r.ok()) {
            MESSAGE("brak kontekstu audio: " << r.error().message);
            return;
        }
        int defaults = 0;
        for (const auto& d : r.value()) {
            CHECK_FALSE(d.id.empty());
            for (unsigned char ch : d.name) CHECK(ch >= 0x20);
            for (unsigned char ch : d.id) CHECK(ch >= 0x20);
            if (d.isDefault) ++defaults;
        }
        CHECK(defaults <= 1);
        if (!r.value().empty()) {
            // Trwalosc id: druga lista daje te same id.
            auto r2 = audio_out::listDevices();
            REQUIRE(r2.ok());
            REQUIRE(r2.value().size() == r.value().size());
            for (std::size_t i = 0; i < r.value().size(); ++i) CHECK(r.value()[i].id == r2.value()[i].id);
        }
    }

    TEST_CASE("Config::deviceId na backendzie null: puste, znane i nieznane id; reopen zachowuje konfiguracje")
    {
        PcmRing ring;
        audio_out::AudioOut out(ring);
        audio_out::Config cfg;
        cfg.backend = audio_out::Config::Backend::Null;
        cfg.sampleRate = 48000;

        auto r0 = out.open(cfg);                      // puste = domyslne
        REQUIRE(r0.ok());
        CHECK(r0.value().deviceId.empty());
        CHECK_FALSE(r0.value().fellBackToDefault);

        cfg.deviceId = "null";                        // znane
        auto r1 = out.open(cfg);
        REQUIRE(r1.ok());
        CHECK(r1.value().deviceId == "null");
        CHECK_FALSE(r1.value().fellBackToDefault);
        auto rr = out.reopen();
        REQUIRE(rr.ok());
        CHECK(rr.value().deviceId == "null");
        CHECK_FALSE(rr.value().fellBackToDefault);

        cfg.deviceId = "nie-ma-takiego-urzadzenia";   // nieznane -> domyslne, nie blad
        auto r2 = out.open(cfg);
        REQUIRE(r2.ok());
        CHECK(out.isOpen());
        CHECK(r2.value().fellBackToDefault);
        CHECK(r2.value().deviceId.empty());
        auto r3 = out.reopen();                       // ta sama konfiguracja -> ten sam fallback
        REQUIRE(r3.ok());
        CHECK(r3.value().fellBackToDefault);
    }

    TEST_CASE("start bez open() zwraca blad, nie wyjatek")
    {
        PcmRing ring;
        audio_out::AudioOut out(ring);
        CHECK_FALSE(out.start().ok());
        out.stop();
        out.close();
    }

    TEST_CASE("prawdziwe urzadzenie (WASAPI): opoznienie" * doctest::skip(true))
    {
        // Uruchamiane tylko z VGMPLAY_TEST_AUDIO=1 (patrz nizej) - pomiar informacyjny.
    }

    TEST_CASE("WASAPI shared: otwarcie, parametry i opoznienie bufora (VGMPLAY_TEST_AUDIO=1)")
    {
        char* env = nullptr;
        std::size_t len = 0;
        _dupenv_s(&env, &len, "VGMPLAY_TEST_AUDIO");
        const bool enabled = env && env[0] == '1';
        std::free(env);
        if (!enabled) { MESSAGE("pominiete: ustaw VGMPLAY_TEST_AUDIO=1"); return; }

        PcmRing ring;
        audio_out::AudioOut out(ring);
        auto r = out.open({});
        REQUIRE(r.ok());
        const auto& i = r.value();
        std::fprintf(stderr,
            "[audio] backend=%s device=\"%s\" rate=%u ch=%u period=%u frames x %u periods => bufor %.1f ms\n",
            i.backend.c_str(), i.name.c_str(), i.sampleRate, i.channels, i.periodFrames, i.periods, i.bufferMs);
        REQUIRE(out.start().ok());
        ring.beginEpoch(1);
        ring.setActive(true);
        // 300 ms cichego tonu; zmierz czas od pierwszej publikacji do pierwszej klatki zuzytej
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t start = 0;
        double firstConsumedMs = -1;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) {
            if (publishConst(ring, 1, start, 480, 0.02f)) start += 480;
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (firstConsumedMs < 0 && ring.consumedFrames() > 0)
                firstConsumedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        std::fprintf(stderr, "[audio] pierwsza klatka zuzyta przez callback po %.1f ms od startu; underruns=%u\n",
                     firstConsumedMs, ring.underruns());
        out.stop();
        CHECK(firstConsumedMs >= 0);
        CHECK(i.sampleRate > 0);
    }
}
