// Testy modulu engine: os czasu, dekoder VGM (libvgm), Engine + PcmRing.
// Utwory sa syntetyczne (SN76489 w wersji 1.51) budowane w kodzie - bez plikow zewnetrznych.
// Brak modulow format/loader na tej galezi: LoadedTrack tworzymy bezposrednio z pol struktury.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "vgmplay/engine.hpp"
#include "vgmplay/pcm_ring.hpp"
#include "vgmplay/timeline.hpp"
#include "vgmplay/vgm_decoder.hpp"

using namespace vgmplay;
using namespace vgmplay::engine;

namespace {

// ------------------------------------------------------------------------- syntetyczny VGM
constexpr std::uint32_t kSnClock = 3579545;

void le32(std::vector<std::uint8_t>& v, std::size_t pos, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[pos + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

struct Prog {
    std::vector<std::uint8_t> d;
    std::uint32_t ticks = 0;
    Prog& sn(std::uint8_t b) { d.push_back(0x50); d.push_back(b); return *this; }
    // Ton na kanale 0 o podziale `n` (f = clock / (32 n)), glosnosc maksymalna; reszta kanalow wyciszona.
    Prog& tone(unsigned n)
    {
        sn(0x80 | (n & 0xF)).sn((n >> 4) & 0x3F).sn(0x90);
        sn(0xBF).sn(0xDF).sn(0xFF);
        return *this;
    }
    Prog& silence() { return sn(0x9F); }
    Prog& wait(std::uint32_t t)
    {
        ticks += t;
        while (t > 0) {
            const std::uint32_t n = std::min<std::uint32_t>(t, 0xFFFF);
            d.push_back(0x61); d.push_back(static_cast<std::uint8_t>(n & 0xFF)); d.push_back(static_cast<std::uint8_t>(n >> 8));
            t -= n;
        }
        return *this;
    }
};

struct Vgm {
    std::vector<std::uint8_t> bytes;
    std::uint32_t totalTicks = 0, loopTicks = 0, introTicks = 0;
    bool hasLoop = false;
};

// intro [+ loop]; `loop` puste = brak petli.
Vgm buildVgm(const Prog& intro, const Prog* loop = nullptr)
{
    Vgm out;
    std::vector<std::uint8_t> f(0x40, 0);
    f[0] = 'V'; f[1] = 'g'; f[2] = 'm'; f[3] = ' ';
    le32(f, 0x08, 0x151);
    le32(f, 0x0C, kSnClock);
    f[0x28] = 0x09; f[0x29] = 0x00;     // feedback
    f[0x2A] = 16;                       // shift register width
    le32(f, 0x34, 0x0C);                // dane od 0x40
    f.insert(f.end(), intro.d.begin(), intro.d.end());
    out.introTicks = intro.ticks;
    out.totalTicks = intro.ticks;
    if (loop) {
        le32(f, 0x1C, static_cast<std::uint32_t>(f.size() - 0x1C));
        f.insert(f.end(), loop->d.begin(), loop->d.end());
        out.loopTicks = loop->ticks;
        out.totalTicks += loop->ticks;
        out.hasLoop = true;
        le32(f, 0x20, loop->ticks);
    }
    f.push_back(0x66);
    le32(f, 0x18, out.totalTicks);
    le32(f, 0x04, static_cast<std::uint32_t>(f.size() - 4));
    out.bytes = std::move(f);
    return out;
}

// Ton 440 Hz (N=254) 0.5 s, potem petla: 880 Hz (N=127) 0.75 s + 440 Hz 0.75 s.
Vgm loopTrack()
{
    Prog intro; intro.tone(254).wait(22050);
    Prog loop;  loop.tone(127).wait(33075).tone(254).wait(33075);
    return buildVgm(intro, &loop);
}

// Ton 440 Hz przez 1 s, bez petli.
Vgm plainTrack()
{
    Prog p; p.tone(254).wait(44100).silence();   // key-off: chip bez niego gralby dalej w czasie ciszy ogonowej
    return buildVgm(p);
}

std::shared_ptr<const loader::LoadedTrack> makeTrack(const Vgm& v)
{
    auto t = std::make_shared<loader::LoadedTrack>();
    auto buf = std::make_shared<ByteBuffer>(v.bytes);
    buf->resize(buf->size() + format::kParserPadding, 0);
    t->bytes = buf;
    t->header.versionBcd = 0x151;
    t->header.fileSize = v.bytes.size();
    t->header.totalSamples = v.totalTicks;
    t->header.loopSamples = v.loopTicks;
    t->header.loopOffset = v.hasLoop ? 0x40 + 1 : 0;      // != 0 wystarcza dla hasLoop()
    t->header.dataOffset = 0x40;
    t->header.dataEnd = v.bytes.size();
    t->stream.totalSamples = v.totalTicks;
    t->stream.loopStartSample = v.introTicks;
    t->stream.hasEndCommand = true;
    return t;
}

// ------------------------------------------------------------------------- analiza sygnalu
double rms(const std::vector<float>& s, std::size_t fromFrame, std::size_t toFrame)
{
    double acc = 0;
    std::size_t n = 0;
    for (std::size_t i = fromFrame; i < toFrame && 2 * i + 1 < s.size(); ++i, ++n) {
        const double l = s[2 * i], r = s[2 * i + 1];
        acc += 0.5 * (l * l + r * r);
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

double peak(const std::vector<float>& s, std::size_t fromFrame, std::size_t toFrame)
{
    double p = 0;
    for (std::size_t i = fromFrame; i < toFrame && 2 * i + 1 < s.size(); ++i)
        p = std::max({p, static_cast<double>(std::fabs(s[2 * i])), static_cast<double>(std::fabs(s[2 * i + 1]))});
    return p;
}

// Czestotliwosc z przejsc przez zero (kanal L, srednia po oknie); wystarcza dla fali prostokatnej.
double frequency(const std::vector<float>& s, std::size_t fromFrame, std::size_t toFrame, double rate)
{
    std::size_t crossings = 0;
    float prev = 0;
    bool havePrev = false;
    // prog histerezy: nie licz drgan wokol zera
    int sign = 0;
    for (std::size_t i = fromFrame; i < toFrame && 2 * i < s.size(); ++i) {
        const float v = s[2 * i];
        const int ns = v > 0.01f ? 1 : (v < -0.01f ? -1 : 0);
        if (ns != 0) {
            if (sign != 0 && ns != sign) ++crossings;
            sign = ns;
        }
        (void)prev; (void)havePrev;
    }
    const double secs = static_cast<double>(toFrame - fromFrame) / rate;
    return secs > 0 ? 0.5 * static_cast<double>(crossings) / secs : 0.0;
}

// Renderuje dekoderem do konca (lub limitu); zwraca interleaved L,R.
std::vector<float> renderAll(IDecoder& dec, std::uint64_t maxFrames = 44100u * 60, std::uint32_t block = 512)
{
    std::vector<float> out;
    std::vector<float> buf(2u * kMaxBlockFrames);
    for (;;) {
        const RenderResult r = dec.render(buf.data(), block, nullptr);
        out.insert(out.end(), buf.begin(), buf.begin() + 2u * r.frames);
        if (r.ended || out.size() / 2 >= maxFrames) break;
    }
    return out;
}

DecoderConfig decCfg(std::uint32_t rate = 44100)
{
    DecoderConfig c;
    c.sampleRate = rate;
    return c;
}

Result<TrackInfo> openStart(IDecoder& dec, const Vgm& v, const DecoderConfig& cfg)
{
    auto info = dec.open(v.bytes, cfg);
    if (info.ok()) {
        auto s = dec.start();
        if (!s.ok()) return s.error();
    }
    return info;
}

} // namespace

// =============================================================================================
TEST_SUITE("engine/timeline")
{
    TEST_CASE("modifiedLoopCount jak VGMPlayer::GetModifiedLoopCount")
    {
        CHECK(modifiedLoopCount(2, 0, 0) == 2);
        CHECK(modifiedLoopCount(0, 0, 0) == 0);
        CHECK(modifiedLoopCount(2, 0, 0x08) == 1);       // 0.5x
        CHECK(modifiedLoopCount(2, 0, 0x20) == 4);       // 2x
        CHECK(modifiedLoopCount(2, 1, 0) == 1);          // base odejmowane
        CHECK(modifiedLoopCount(2, 5, 0) == 1);          // <= base => 1
        CHECK(modifiedLoopCount(4, 1, 0) == 3);
    }

    TEST_CASE("computeTimeline: petla, bez petli, nieskonczony")
    {
        TimelineInput in;
        in.sampleRate = 44100;
        in.totalTicks = 88200; in.loopStartTick = 22050; in.hasLoop = true;
        in.loopCount = 2; in.fadeMs = 1000;
        PlaybackTimeline t = computeTimeline(in);
        CHECK(t.introFrames == 22050);
        CHECK(t.loopFrames == 66150);
        CHECK(t.fadeStartFrame == 22050 + 2 * 66150);
        CHECK(t.totalFrames == t.fadeStartFrame + 44100);
        CHECK(t.seekLimit == t.fadeStartFrame);

        in.loopCount = 0;
        t = computeTimeline(in);
        CHECK(t.infinite);
        CHECK(t.totalFrames == 0);

        in.hasLoop = false; in.loopCount = 2; in.totalTicks = 44100;
        t = computeTimeline(in);
        CHECK(t.totalFrames == 44100 + 22050);     // + 500 ms ciszy ogonowej
        CHECK(t.fadeFrames == 0);
        in.nonLoopEnd = NonLoopEnd::FadeOut;
        t = computeTimeline(in);
        CHECK(t.totalFrames == 44100);
        CHECK(t.fadeFrames == 44100);
        CHECK(t.fadeStartFrame == 0);
    }
}

// =============================================================================================
TEST_SUITE("engine/decoder")
{
    TEST_CASE("open odrzuca zle wejscie")
    {
        auto dec = makeVgmDecoder();
        std::vector<std::uint8_t> tiny(10, 0);
        CHECK_FALSE(dec->open(tiny, decCfg()).ok());
        std::vector<std::uint8_t> badSig(0x80, 0);
        auto r = dec->open(badSig, decCfg());
        REQUIRE_FALSE(r.ok());
        CHECK(r.error().code == ErrorCode::BadSignature);
        DecoderConfig bad = decCfg(10);
        auto v = plainTrack();
        CHECK_FALSE(dec->open(v.bytes, bad).ok());
        // render/seek bez start() nie wywracaja sie
        float buf[16];
        CHECK(dec->render(buf, 8, nullptr).frames == 0);
        CHECK_FALSE(dec->seek(0).ok());
    }

    TEST_CASE("open: metadane, layout i rdzenie")
    {
        auto dec = makeVgmDecoder();
        const Vgm v = loopTrack();
        auto info = dec->open(v.bytes, decCfg());
        REQUIRE(info.ok());
        const TrackInfo& ti = info.value();
        CHECK(ti.format == "VGM");
        CHECK(ti.versionBcd == 0x151);
        CHECK(ti.totalFrames == 88200);
        CHECK(ti.introFrames == 22050);
        CHECK(ti.loopFrames == 66150);
        REQUIRE(ti.layout.devices.size() == 1);
        const DeviceDesc& d = ti.layout.devices[0];
        CHECK(d.type == ChipType::SN76496);
        CHECK(d.name == "SN76496");
        CHECK(d.channelCount == 4);
        CHECK(d.mainChannelCount == 4);
        CHECK(d.channelNames.size() == 4);
        CHECK_FALSE(d.hasLinked);
        CHECK(d.coreId == 0x4D414D45);      // MAME (FCC)
        CHECK(ti.layout.channels.size() == 4);
        CHECK(dec->start().ok());
        CHECK(dec->availableCores(ChipType::YM2612).size() >= 2);
        CHECK(dec->chipState() == nullptr);
    }

    TEST_CASE("polityka rdzeni: rdzenie wylaczone przez licencje nie sa dostepne, domyslne sa jawne")
    {
        auto dec = makeVgmDecoder();
        auto has = [&](ChipType t, std::uint32_t fcc) {
            for (const auto& c : dec->availableCores(t)) if (c.coreId == fcc) return true;
            return false;
        };
        constexpr std::uint32_t MAME = 0x4D414D45, ADLE = 0x41444C45, OOTK = 0x4F4F544B, GPGX = 0x47504758;
        CHECK(has(ChipType::YM3812, MAME));
        CHECK_FALSE(has(ChipType::YM3812, ADLE));
        CHECK_FALSE(has(ChipType::YMF262, ADLE));
        CHECK(has(ChipType::HuC6280, MAME));
        CHECK_FALSE(has(ChipType::HuC6280, OOTK));
        CHECK(has(ChipType::YM2612, GPGX));
        CHECK(has(ChipType::QSound, MAME));
        CHECK(has(ChipType::RF5C164, MAME));
        CHECK(has(ChipType::YMF262, MAME));
    }

    TEST_CASE("wszystkie ukladzy z listy VGM maja deklaracje libvgm (z wyjatkiem zaslepki ES5506)")
    {
        auto dec = makeVgmDecoder();
        int missing = 0;
        for (unsigned t = 1; t < static_cast<unsigned>(ChipType::Count_); ++t) {
            const auto type = static_cast<ChipType>(t);
            if (type == ChipType::ES5506) continue;        // deklaracja bez rdzeni
            if (t == 50) continue;                         // YMW258: duplikat MultiPCM (usuniety w kontrakcie po scaleniu)
            if (dec->availableCores(type).empty()) { ++missing; MESSAGE("brak rdzeni: typ ", t); }
        }
        CHECK(missing == 0);
    }

    TEST_CASE("OPL4: czesc FM (polaczony YMF262) gra mimo wylaczonego adlibemu")
    {
        // Pulapka z ADR 0002 pkt 7: libvgm wymusza FCC_ADLE dla FM w OPL4; bez emuCore[1] FM milczy.
        std::vector<std::uint8_t> f(0x80, 0);
        f[0] = 'V'; f[1] = 'g'; f[2] = 'm'; f[3] = ' ';
        le32(f, 0x08, 0x161);
        le32(f, 0x34, 0x4C);                 // dane od 0x80
        le32(f, 0x60, 33868800);             // YMF278B
        auto fm = [&](std::uint8_t reg, std::uint8_t val) {
            f.push_back(0xD0); f.push_back(0x00); f.push_back(reg); f.push_back(val);
        };
        fm(0x20, 0x01); fm(0x23, 0x01); fm(0x40, 0x10); fm(0x43, 0x00);
        fm(0x60, 0xF0); fm(0x63, 0xF0); fm(0x80, 0x77); fm(0x83, 0x77);
        fm(0xC0, 0x31); fm(0xA0, 0x98); fm(0xB0, 0x31);
        f.push_back(0x61); f.push_back(0x44); f.push_back(0xAC);    // 44100 ticks
        f.push_back(0x66);
        le32(f, 0x18, 44100);
        le32(f, 0x04, static_cast<std::uint32_t>(f.size() - 4));
        auto dec = makeVgmDecoder();
        auto info = dec->open(f, decCfg());
        REQUIRE(info.ok());
        REQUIRE(info.value().layout.devices.size() == 1);
        const DeviceDesc& d = info.value().layout.devices[0];
        CHECK(d.type == ChipType::YMF278B);
        CHECK(d.hasLinked);
        CHECK(d.channelCount > d.mainChannelCount);
        REQUIRE(dec->start().ok());
        const auto pcm = renderAll(*dec, 30000);
        CHECK(peak(pcm, 5000, 20000) > 0.001);
    }

    TEST_CASE("utwor bez petli konczy sie bez wyciszania, z cisza ogonowa")
    {
        auto dec = makeVgmDecoder();
        const Vgm v = plainTrack();
        REQUIRE(openStart(*dec, v, decCfg()).ok());
        const auto pcm = renderAll(*dec);
        const std::size_t frames = pcm.size() / 2;
        // 1 s tonu + 0,5 s ciszy ogonowej (+- blok)
        CHECK(frames >= 44100 + 22050 - 600);
        CHECK(frames <= 44100 + 22050 + 600);
        const double mid = rms(pcm, 20000, 24000);
        const double endOfTone = rms(pcm, 44100 - 2000, 44100 - 200);
        REQUIRE(mid > 0.01);
        CHECK(endOfTone > 0.9 * mid);                   // brak wyciszania
        CHECK(peak(pcm, 44100 + 200, frames) < 1e-4);   // cisza po utworze
    }

    TEST_CASE("utwor z petla: 2 przejscia, wyciszenie 1 s, dlugosc zgodna z os czasu")
    {
        auto dec = makeVgmDecoder();
        const Vgm v = loopTrack();
        REQUIRE(openStart(*dec, v, decCfg()).ok());
        const auto pcm = renderAll(*dec);
        const std::size_t frames = pcm.size() / 2;
        TimelineInput in;
        in.totalTicks = v.totalTicks; in.loopStartTick = v.introTicks; in.hasLoop = true;
        in.sampleRate = 44100;
        const PlaybackTimeline t = computeTimeline(in);
        CHECK(frames + 600 >= t.totalFrames);
        CHECK(frames <= t.totalFrames + 600);
        // przed wyciszeniem: pelna glosnosc; w srodku wyciszenia: wyraznie ciszej; na koncu ~0
        const double full = rms(pcm, 30000, 34000);
        const double fadeStartRms = rms(pcm, t.fadeStartFrame - 3000, t.fadeStartFrame - 200);
        const double fadeMid = rms(pcm, t.fadeStartFrame + 20000, t.fadeStartFrame + 24000);
        const double fadeEnd = rms(pcm, t.totalFrames - 1500, t.totalFrames - 100);
        REQUIRE(full > 0.01);
        CHECK(fadeStartRms > 0.9 * full);
        CHECK(fadeMid < 0.5 * full);
        CHECK(fadeEnd < 0.05 * full);
    }

    TEST_CASE("loopCount i fade z konfiguracji")
    {
        const Vgm v = loopTrack();
        auto len = [&](std::uint32_t loops, std::uint32_t fadeMs) {
            auto dec = makeVgmDecoder();
            DecoderConfig c = decCfg();
            c.loopCount = loops; c.fadeMs = fadeMs;
            REQUIRE(openStart(*dec, v, c).ok());
            return renderAll(*dec).size() / 2;
        };
        const std::size_t l1 = len(1, 1000), l3 = len(3, 1000), f500 = len(2, 500);
        CHECK(l3 > l1);
        CHECK(l3 - l1 >= 2 * 66150 - 1200);
        CHECK(l3 - l1 <= 2 * 66150 + 1200);
        CHECK(len(2, 1000) - f500 >= 22050 - 1200);
    }

    TEST_CASE("pelne nieskonczone petle: render nie konczy sie sam")
    {
        auto dec = makeVgmDecoder();
        DecoderConfig c = decCfg();
        c.loopCount = 0;
        REQUIRE(openStart(*dec, loopTrack(), c).ok());
        const auto pcm = renderAll(*dec, 44100u * 12);
        CHECK(pcm.size() / 2 >= 44100u * 12);
        CHECK(rms(pcm, 44100u * 11, 44100u * 12) > 0.01);
    }

    TEST_CASE("seek(X) ~ render 0->X: pozycja i charakter dzwieku")
    {
        const Vgm v = loopTrack();
        // wzorzec: render od zera
        auto ref = makeVgmDecoder();
        REQUIRE(openStart(*ref, v, decCfg()).ok());
        const auto full = renderAll(*ref, 44100u * 4);

        auto dec = makeVgmDecoder();
        REQUIRE(openStart(*dec, v, decCfg()).ok());
        const SampleTime target = 22050 + 10000;            // w pierwszej czesci petli (880 Hz)
        REQUIRE(dec->seek(target).ok());
        CHECK(dec->positionFrames() + 2 >= target);
        CHECK(dec->positionFrames() <= target + 2);
        std::vector<float> buf(2u * 512);
        std::vector<float> after;
        for (int i = 0; i < 40; ++i) {
            const RenderResult r = dec->render(buf.data(), 512, nullptr);
            after.insert(after.end(), buf.begin(), buf.begin() + 2u * r.frames);
        }
        const double fRef = frequency(full, static_cast<std::size_t>(target), static_cast<std::size_t>(target) + 20000, 44100);
        const double fSeek = frequency(after, 0, 20000, 44100);
        CHECK(fRef == doctest::Approx(880.0).epsilon(0.03));
        CHECK(fSeek == doctest::Approx(fRef).epsilon(0.03));
        CHECK(rms(after, 0, 20000) == doctest::Approx(rms(full, static_cast<std::size_t>(target), static_cast<std::size_t>(target) + 20000)).epsilon(0.05));

        // seek wstecz (Reset + powtorka) i w przod w obrebie petli
        REQUIRE(dec->seek(1000).ok());
        CHECK(dec->positionFrames() + 2 >= 1000);
        CHECK(dec->positionFrames() <= 1002);
        REQUIRE(dec->seek(22050 + 66150 + 5000).ok());        // druga iteracja petli
        const RenderResult r = dec->render(buf.data(), 512, nullptr);
        CHECK(r.frames == 512);
        CHECK_FALSE(r.ended);
    }

    TEST_CASE("mute: kanal wyciszony daje cisze, odciszenie przywraca dzwiek; start zeruje maski")
    {
        auto dec = makeVgmDecoder();
        REQUIRE(openStart(*dec, plainTrack(), decCfg()).ok());
        std::vector<float> buf(2u * 512);
        (void)dec->render(buf.data(), 512, nullptr);
        dec->setMute(0, MuteMask{0x1, 0});
        std::vector<float> muted;
        for (int i = 0; i < 8; ++i) {
            const RenderResult r = dec->render(buf.data(), 512, nullptr);
            muted.insert(muted.end(), buf.begin(), buf.begin() + 2u * r.frames);
        }
        CHECK(peak(muted, 600, muted.size() / 2) < 1e-4);     // pierwszy blok: przejscie
        dec->setMute(0, MuteMask{});
        std::vector<float> on;
        for (int i = 0; i < 8; ++i) {
            const RenderResult r = dec->render(buf.data(), 512, nullptr);
            on.insert(on.end(), buf.begin(), buf.begin() + 2u * r.frames);
        }
        CHECK(peak(on, 0, on.size() / 2) > 0.05);

        // maska ustawiona przed start() jest zerowana przez start()
        auto dec2 = makeVgmDecoder();
        const Vgm v = plainTrack();
        REQUIRE(dec2->open(v.bytes, decCfg()).ok());
        dec2->setMute(0, MuteMask{0xF, 0});
        REQUIRE(dec2->start().ok());
        const auto pcm = renderAll(*dec2, 8192);
        CHECK(peak(pcm, 1000, 8000) > 0.05);
        dec2->setMute(5, MuteMask{1, 0});           // zly indeks: ignorowany
    }

    TEST_CASE("render nie przekracza maxFrames i wypelnia scope")
    {
        auto dec = makeVgmDecoder();
        REQUIRE(openStart(*dec, plainTrack(), decCfg()).ok());
        std::vector<float> buf(2u * 512, 123.0f);
        ScopeBlock* sb = new ScopeBlock();
        const RenderResult r = dec->render(buf.data(), 256, sb);
        REQUIRE(r.frames == 256);
        CHECK(sb->frames == 256);
        CHECK(buf[2 * 256] == 123.0f);           // poza maxFrames bufor nietkniety
        for (std::uint32_t i = 0; i < 256; ++i) {
            CHECK(sb->masterL[i] == buf[2 * i]);
            CHECK(sb->masterR[i] == buf[2 * i + 1]);
        }
        delete sb;
    }

    TEST_CASE("inne czestotliwosci wyjscia daja te sama dlugosc w sekundach")
    {
        const Vgm v = plainTrack();
        for (std::uint32_t sr : {44100u, 48000u, 96000u}) {
            auto dec = makeVgmDecoder();
            REQUIRE(openStart(*dec, v, decCfg(sr)).ok());
            const auto pcm = renderAll(*dec, 96000u * 5);
            const double secs = static_cast<double>(pcm.size() / 2) / sr;
            CHECK(secs == doctest::Approx(1.5).epsilon(0.02));
        }
    }

    TEST_CASE("koszt renderu i czas seeka (pomiar informacyjny)")
    {
        // 5 minut: ton + powtarzane zmiany tonu (duzo komend)
        Prog p;
        for (int i = 0; i < 3000; ++i) { p.tone(100 + (i % 50)).wait(4410); }
        const Vgm v = buildVgm(p);
        auto dec = makeVgmDecoder();
        REQUIRE(openStart(*dec, v, decCfg(48000)).ok());
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        std::vector<float> buf(2u * 512);
        std::uint64_t frames = 0;
        while (frames < 48000u * 30) frames += dec->render(buf.data(), 512, nullptr).frames;
        const double wall = std::chrono::duration<double>(clk::now() - t0).count();
        MESSAGE("render 30 s @48 kHz (SN76489): ", wall * 1000, " ms (x", 30.0 / wall, " realtime)");
        CHECK(wall < 30.0);          // z zapasem; liczba informacyjna
        t0 = clk::now();
        REQUIRE(dec->seek(48000u * 280).ok());           // w przod, przez ~2800 zmian tonu
        const double fwd = std::chrono::duration<double>(clk::now() - t0).count();
        t0 = clk::now();
        REQUIRE(dec->seek(48000u * 5).ok());             // wstecz: Reset + powtorka
        const double back = std::chrono::duration<double>(clk::now() - t0).count();
        MESSAGE("seek w przod do 280 s: ", fwd * 1000, " ms; wstecz do 5 s: ", back * 1000, " ms");
        CHECK(fwd < 2.0);
        CHECK(back < 2.0);
    }
}

// =============================================================================================
namespace {

// Symuluje callback audio: konsumuje z PcmRing z predkoscia `speed` x czas rzeczywisty i
// opcjonalnie nagrywa wynik.
struct Rig {
    PcmRing pcm;
    std::unique_ptr<Engine> eng;
    std::thread th;
    std::vector<float> rec;
    bool recording = false;
    std::uint32_t rate;
    double speed;
    double owed = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::vector<EngineEvent> events;

    explicit Rig(std::uint32_t sr = 48000, double speedFactor = 4.0, std::uint32_t loops = 2,
                 std::uint32_t fadeMs = 1000)
        : rate(sr), speed(speedFactor)
    {
        EngineConfig cfg;
        cfg.decoder.sampleRate = sr;
        cfg.decoder.loopCount = loops;
        cfg.decoder.fadeMs = fadeMs;
        eng = std::make_unique<Engine>(cfg, pcm);
        th = std::thread([this] { eng->run(); });
    }
    ~Rig()
    {
        eng->requestStop();
        th.join();
    }

    void post(Command c)
    {
        while (!eng->post(c)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    void drainEvents()
    {
        EngineEvent e;
        while (eng->pollEvent(e)) events.push_back(std::move(e));
    }

    template <class T> int count() const
    {
        int n = 0;
        for (const auto& e : events) if (std::holds_alternative<T>(e)) ++n;
        return n;
    }

    // Konsumuje przez `ms` czasu rzeczywistego.
    void run(int ms)
    {
        using clk = std::chrono::steady_clock;
        const auto start = clk::now();
        last = std::chrono::steady_clock::now();
        std::vector<float> buf(2u * 512);
        while (clk::now() - start < std::chrono::milliseconds(ms)) {
            const auto now = clk::now();
            owed += std::chrono::duration<double>(now - last).count() * rate * speed;
            last = now;
            while (owed >= 256) {
                pcm.consume(buf.data(), 256);
                if (recording) rec.insert(rec.end(), buf.begin(), buf.begin() + 512);
                owed -= 256;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        drainEvents();
    }

    template <class Pred> bool until(Pred p, int timeoutMs)
    {
        using clk = std::chrono::steady_clock;
        const auto start = clk::now();
        while (clk::now() - start < std::chrono::milliseconds(timeoutMs)) {
            run(5);
            if (p()) return true;
        }
        return false;
    }
};

} // namespace

TEST_SUITE("engine/engine")
{
    TEST_CASE("Load + Play do konca: zdarzenia, dlugosc, brak wyciszania (bez petli)")
    {
        Rig rig;
        rig.post(cmd::Load{makeTrack(plainTrack()), true});
        REQUIRE(rig.until([&] { return rig.count<evt::TrackEnded>() > 0; }, 10000));
        rig.run(200);
        CHECK(rig.count<evt::TrackLoaded>() == 1);
        CHECK(rig.count<evt::Error>() == 0);
        const TransportSnapshot s = rig.eng->snapshot();
        CHECK(s.state == TransportState::Ended);
        CHECK(s.totalFrames == 48000 + 24000);
        const std::uint64_t consumed = rig.pcm.consumedFrames();
        CHECK(consumed + 1024 >= 72000);
        CHECK(consumed <= 72000 + 1024);
        // TrackLoaded niesie dane z LoadedTrack
        for (const auto& e : rig.events) {
            if (const auto* tl = std::get_if<evt::TrackLoaded>(&e)) {
                REQUIRE(tl->info);
                CHECK(tl->info->totalFrames == 48000);
                CHECK(tl->info->layout.devices.size() == 1);
            }
        }
    }

    TEST_CASE("petla: dlugosc = intro + 2 petle + wyciszenie; loopIndex i fadeActive")
    {
        Rig rig;
        rig.recording = true;
        rig.post(cmd::Load{makeTrack(loopTrack()), true});
        bool sawFade = false;
        std::uint32_t maxLoop = 0;
        REQUIRE(rig.until([&] {
            const auto s = rig.eng->snapshot();
            sawFade = sawFade || s.fadeActive;
            maxLoop = std::max(maxLoop, s.loopIndex);
            return rig.count<evt::TrackEnded>() > 0;
        }, 20000));
        rig.run(300);
        const std::uint64_t expected = 24000 + 2ull * 72000 + 48000;      // 0,5 s + 2x1,5 s + 1 s @48 kHz
        CHECK(rig.pcm.consumedFrames() + 1024 >= expected);
        CHECK(rig.pcm.consumedFrames() <= expected + 1024);
        CHECK(sawFade);
        CHECK(maxLoop >= 2);
        CHECK(rig.eng->snapshot().totalFrames == expected);
    }

    TEST_CASE("Seek: nowa epoka, pozycja slyszana dochodzi do celu, brak cofania sie")
    {
        Rig rig(48000, 1.0);
        rig.post(cmd::Load{makeTrack(loopTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 20000; }, 5000));
        const std::uint64_t e0 = rig.eng->snapshot().epoch;
        const SampleTime target = 48000 * 2;
        rig.post(cmd::Seek{target});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().epoch > e0; }, 2000));
        auto s = rig.eng->snapshot();
        // zaraz po seeku (przed odtworzeniem pierwszego bloku) pozycja = cel, nie stara pozycja
        CHECK(s.audibleFrame + 1 >= target);
        CHECK(s.audibleFrame < target + 24000);
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > target + 4800; }, 4000));
        s = rig.eng->snapshot();
        CHECK(s.audibleFrame < target + 48000);
        CHECK(s.state == TransportState::Playing);
        // Seek poza koniec ostatniej petli jest ograniczany
        rig.post(cmd::Seek{48000ull * 100});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().epoch > s.epoch; }, 2000));
        rig.run(100);
        CHECK(rig.eng->snapshot().renderedFrame < 24000u + 2u * 72000u + 15000u);   // limit + ~100 ms renderu
    }

    TEST_CASE("Pause zatrzymuje render, Play wznawia od tej samej pozycji")
    {
        Rig rig(48000, 1.0);
        rig.post(cmd::Load{makeTrack(loopTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 24000; }, 5000));
        rig.post(cmd::Pause{});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().state == TransportState::Paused; }, 2000));
        rig.run(400);                                     // pierscien sie oproznia
        const auto p1 = rig.eng->snapshot();
        const std::uint64_t consumed1 = rig.pcm.consumedFrames();
        rig.run(300);
        const auto p2 = rig.eng->snapshot();
        CHECK(rig.pcm.consumedFrames() == consumed1);
        CHECK(p2.renderedFrame == p1.renderedFrame);
        CHECK(p2.underruns == p1.underruns);              // cisza podczas pauzy nie jest underrunem
        // pozycja slyszana dogonila renderowana (z dokladnoscia do bloku)
        CHECK(p2.audibleFrame + 600 >= p2.renderedFrame);
        rig.post(cmd::Play{});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > p2.audibleFrame + 4800; }, 3000));
        const auto p3 = rig.eng->snapshot();
        CHECK(p3.state == TransportState::Playing);
        CHECK(p3.audibleFrame >= p2.audibleFrame);
    }

    TEST_CASE("Stop wraca na poczatek, Play po koncu startuje od nowa")
    {
        Rig rig;
        rig.post(cmd::Load{makeTrack(plainTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 10000; }, 5000));
        rig.post(cmd::Stop{});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().state == TransportState::Stopped; }, 2000));
        rig.run(100);
        CHECK(rig.eng->snapshot().renderedFrame == 0);
        CHECK(rig.eng->snapshot().audibleFrame == 0);
        rig.post(cmd::Play{});
        REQUIRE(rig.until([&] { return rig.count<evt::TrackEnded>() > 0; }, 10000));
        rig.post(cmd::Play{});          // po koncu: od nowa
        REQUIRE(rig.until([&] { return rig.eng->snapshot().state == TransportState::Playing; }, 2000));
        REQUIRE(rig.until([&] { return rig.count<evt::TrackEnded>() > 1; }, 10000));
    }

    TEST_CASE("Mute i solo przez Engine; maski zerowane przy Load")
    {
        Rig rig(48000, 1.0, 0);   // bez konca: test dluzszy niz utwor
        rig.recording = true;
        const auto tr = makeTrack(loopTrack());
        rig.post(cmd::Load{tr, true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 4800; }, 5000));
        rig.post(cmd::SetMute{0, MuteMask{0x1, 0}});
        rig.run(500);                                     // opoznienie bufora
        rig.rec.clear();
        rig.run(300);
        REQUIRE(rig.rec.size() / 2 > 4000);
        CHECK(peak(rig.rec, 0, rig.rec.size() / 2) < 1e-4);

        // solo na kanale 0 odtwarza tylko jego (inne i tak milcza) - ton wraca po zdjeciu mute
        rig.post(cmd::SetMute{0, MuteMask{}});
        rig.post(cmd::SetSolo{0, 0, true});
        rig.run(500);
        rig.rec.clear();
        rig.run(300);
        CHECK(peak(rig.rec, 0, rig.rec.size() / 2) > 0.05);
        // solo na kanale 1: kanal 0 wyciszony
        rig.post(cmd::SetSolo{0, 0, false});
        rig.post(cmd::SetSolo{0, 1, true});
        rig.run(500);
        rig.rec.clear();
        rig.run(300);
        CHECK(peak(rig.rec, 0, rig.rec.size() / 2) < 1e-4);
        // zdjecie solo
        rig.post(cmd::SetSolo{0, kChipSumChannel, false});
        rig.run(500);
        rig.rec.clear();
        rig.run(300);
        CHECK(peak(rig.rec, 0, rig.rec.size() / 2) > 0.05);

        // maska + Load: nowy utwor gra mimo wczesniejszego mute
        rig.post(cmd::SetMute{0, MuteMask{0xF, 0}});
        rig.run(300);
        rig.post(cmd::Load{tr, true});
        rig.run(600);
        rig.rec.clear();
        rig.run(300);
        CHECK(peak(rig.rec, 0, rig.rec.size() / 2) > 0.05);
    }

    TEST_CASE("SetVolume skaluje wyjscie")
    {
        Rig rig(48000, 1.0, 0);   // bez konca: test dluzszy niz utwor
        rig.recording = true;
        rig.post(cmd::Load{makeTrack(loopTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 4800; }, 5000));
        rig.run(300);
        rig.rec.clear();
        rig.run(200);
        const double full = rms(rig.rec, 0, rig.rec.size() / 2);
        rig.post(cmd::SetVolume{0.5f});                   // krzywa x^2 => 0,25
        rig.run(400);
        rig.rec.clear();
        rig.run(200);
        const double half = rms(rig.rec, 0, rig.rec.size() / 2);
        REQUIRE(full > 0.01);
        CHECK(half / full == doctest::Approx(0.25).epsilon(0.1));
    }

    TEST_CASE("SetLoopCount przelicza os czasu bez utraty pozycji")
    {
        Rig rig(48000, 1.0);
        rig.post(cmd::Load{makeTrack(loopTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 24000; }, 5000));
        const std::uint64_t before = rig.eng->snapshot().totalFrames;
        rig.post(cmd::SetLoopCount{1});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().totalFrames != before; }, 3000));
        CHECK(rig.eng->snapshot().totalFrames == 24000 + 72000 + 48000);
        REQUIRE(rig.until([&] { return rig.eng->snapshot().audibleFrame > 30000; }, 3000));
        CHECK(rig.eng->snapshot().state == TransportState::Playing);
        rig.post(cmd::SetLoopCount{0});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().totalFrames == 0; }, 3000));
    }

    TEST_CASE("Load pustego/zlego utworu: Failed + Error, silnik zyje dalej")
    {
        Rig rig;
        rig.post(cmd::Load{nullptr, true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().state == TransportState::Failed; }, 3000));
        CHECK(rig.count<evt::Error>() == 1);

        Vgm bad = plainTrack();
        bad.bytes[0] = 'X';
        rig.post(cmd::Load{makeTrack(bad), true});
        REQUIRE(rig.until([&] { return rig.count<evt::Error>() == 2; }, 3000));
        CHECK(rig.eng->snapshot().state == TransportState::Failed);

        rig.post(cmd::Load{makeTrack(plainTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().state == TransportState::Playing; }, 3000));
    }

    TEST_CASE("kolejka komend: pelna zwraca false, nie gubi poprzednich")
    {
        PcmRing pcm;
        Engine eng(EngineConfig{}, pcm);          // bez watku silnika - nikt nie opruznia kolejki
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < kCommandQueueSize + 50; ++i)
            if (eng.post(cmd::SetVolume{0.5f})) ++accepted;
        CHECK(accepted == kCommandQueueSize);
    }
}
