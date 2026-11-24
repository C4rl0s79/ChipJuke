// Testy E1.5: dzwiek osobno dla kazdego kanalu (kopie emulacji z maska MUTE, ChannelScopeBank).
// Czesc offline (bank + dekoder, deterministyczna, bez watkow) i czesc przez Engine (kolejka scope).
// Utwory syntetyczne: SN76489 (uklad liniowy: suma kanalow == miks). Korpus: tests/data/valid.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_util.hpp"
#include "vgmplay/channel_scopes.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/pcm_ring.hpp"
#include "vgmplay/vgm_decoder.hpp"

using namespace vgmplay;
using namespace vgmplay::engine;

namespace {

constexpr std::uint32_t kSnClock = 3579545;
constexpr std::uint32_t kRate = 48000;

void le32(std::vector<std::uint8_t>& v, std::size_t pos, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[pos + i] = static_cast<std::uint8_t>(x >> (8 * i));
}

// Maly kompilator programow SN76489: trzy tony + szum, glosnosc 0 (maks.) lub wyciszony.
struct Sn {
    std::vector<std::uint8_t> d;
    std::uint32_t ticks = 0;
    Sn& w(std::uint8_t b) { d.push_back(0x50); d.push_back(b); return *this; }
    // ton na kanale ch (0..2) o podziale n; vol 0 = glosno, 15 = cisza
    Sn& tone(unsigned ch, unsigned n, unsigned vol)
    {
        w(static_cast<std::uint8_t>(0x80 | (ch << 5) | (n & 0xF))).w(static_cast<std::uint8_t>((n >> 4) & 0x3F));
        return att(ch, vol);
    }
    Sn& att(unsigned ch, unsigned vol) { return w(static_cast<std::uint8_t>(0x90 | (ch << 5) | (vol & 0xF))); }
    Sn& wait(std::uint32_t t)
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

struct Built {
    std::vector<std::uint8_t> bytes;
    std::uint32_t totalTicks = 0;
};

Built build(const Sn& p)
{
    Built out;
    std::vector<std::uint8_t> f(0x40, 0);
    f[0] = 'V'; f[1] = 'g'; f[2] = 'm'; f[3] = ' ';
    le32(f, 0x08, 0x151);
    le32(f, 0x0C, kSnClock);
    f[0x28] = 0x09; f[0x29] = 0x00;
    f[0x2A] = 16;
    le32(f, 0x34, 0x0C);
    f.insert(f.end(), p.d.begin(), p.d.end());
    f.push_back(0x66);
    le32(f, 0x18, p.ticks);
    le32(f, 0x04, static_cast<std::uint32_t>(f.size() - 4));
    out.bytes = std::move(f);
    out.totalTicks = p.ticks;
    return out;
}

// Fazy po 0,5 s: [ch0 440 Hz] -> [ch1 880 Hz] -> [ch2 1320 Hz]; kazdy kanal gra tylko w swojej fazie.
// Dzieki temu kazdy kanal ma wlasny, rozpoznawalny przebieg i wlasne okno czasu. Koncowa cisza 0,5 s.
Built phasedTrack()
{
    Sn p;
    p.att(1, 15).att(2, 15).att(3, 15);
    p.tone(0, 254, 0).wait(22050);
    p.att(0, 15).tone(1, 127, 0).wait(22050);
    p.att(1, 15).tone(2, 85, 0).wait(22050);
    p.att(2, 15).wait(22050);
    return build(p);
}

// Trzy tony jednoczesnie (test sumy); `seconds` dlugosci (w tickach 44100/s).
Built chordTrack(std::uint32_t seconds = 1)
{
    Sn p;
    p.att(3, 15);
    p.tone(0, 254, 2).tone(1, 127, 2).tone(2, 85, 2).wait(44100 * seconds);
    return build(p);
}

std::shared_ptr<const loader::LoadedTrack> makeTrack(const Built& v)
{
    auto t = std::make_shared<loader::LoadedTrack>();
    auto buf = std::make_shared<ByteBuffer>(v.bytes);
    buf->resize(buf->size() + format::kParserPadding, 0);
    t->bytes = buf;
    t->header.versionBcd = 0x151;
    t->header.fileSize = v.bytes.size();
    t->header.totalSamples = v.totalTicks;
    t->header.dataOffset = 0x40;
    t->header.dataEnd = v.bytes.size();
    t->stream.totalSamples = v.totalTicks;
    t->stream.hasEndCommand = true;
    return t;
}

DecoderConfig cfg48()
{
    DecoderConfig c;
    c.sampleRate = kRate;
    return c;
}

// Dekoder glowny + bank, krok po kroku w tej samej kolejnosci co silnik.
struct Harness {
    std::unique_ptr<IDecoder> main = makeVgmDecoder();
    ChannelScopeBank bank;
    std::shared_ptr<const loader::LoadedTrack> track;
    TrackInfo info;
    SampleTime pos = 0;
    std::vector<float> masterMono;                 // (L+R)/2 mastera
    std::vector<std::vector<float>> ex;            // [strumien][klatka]
    std::vector<ScopeStreamDesc> desc;
    std::unique_ptr<ScopeBlock> sb = std::make_unique<ScopeBlock>();
    std::vector<float> buf = std::vector<float>(2u * kMaxBlockFrames);
    bool ended = false;

    bool open(std::shared_ptr<const loader::LoadedTrack> t, const DecoderConfig& cfg = cfg48(), bool bind = true)
    {
        track = std::move(t);
        auto r = main->open(track->raw(), cfg);
        if (!r.ok()) return false;
        info = std::move(r).value();
        if (!main->start().ok()) return false;
        if (bind) bank.bind(track, cfg, info, *main);
        return true;
    }

    // Synchronizuje wszystkie kopie do biezacej pozycji (jak petla silnika, bez limitu czasu).
    void syncAll()
    {
        int guard = 0;
        while (bank.needsSync() && guard++ < 1000) bank.syncStep(pos, std::chrono::microseconds(1000000));
    }

    void seek(SampleTime target)
    {
        REQUIRE(main->seek(target).ok());
        pos = main->positionFrames();
        bank.invalidate();
        syncAll();
    }

    // Renderuje `frames` klatek w blokach po `block`.
    void run(std::uint64_t frames, std::uint32_t block = 512, bool record = true)
    {
        std::uint64_t left = frames;
        while (left > 0 && !ended) {
            const std::uint32_t want = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, block));
            const RenderResult r = main->render(buf.data(), want, sb.get());
            const std::uint32_t n = r.frames;
            if (n > 0) {
                bank.render(n, pos, sb.get());
                if (record) {
                    if (ex.size() < sb->extraStreamCount) ex.resize(sb->extraStreamCount);
                    desc.assign(sb->extraDesc, sb->extraDesc + sb->extraStreamCount);
                    for (std::uint32_t i = 0; i < n; ++i) masterMono.push_back(0.5f * (sb->masterL[i] + sb->masterR[i]));
                    for (std::uint8_t s = 0; s < sb->extraStreamCount; ++s)
                        ex[s].insert(ex[s].end(), sb->extra[s], sb->extra[s] + n);
                }
                pos += n;
            }
            left -= std::min<std::uint64_t>(left, std::max<std::uint32_t>(n, 1));
            if (r.ended) ended = true;
            if (n == 0) break;
        }
    }
};

double rmsOf(const std::vector<float>& v, std::size_t a, std::size_t b)
{
    b = std::min(b, v.size());
    if (a >= b) return 0;
    double s = 0;
    for (std::size_t i = a; i < b; ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

double peakOf(const std::vector<float>& v, std::size_t a, std::size_t b)
{
    b = std::min(b, v.size());
    double p = 0;
    for (std::size_t i = a; i < b; ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
    return p;
}

// Czestotliwosc z przejsc przez zero (z histereza) - wystarcza dla fali prostokatnej.
double freqOf(const std::vector<float>& v, std::size_t a, std::size_t b, double rate = kRate)
{
    b = std::min(b, v.size());
    int sign = 0;
    std::size_t crossings = 0;
    for (std::size_t i = a; i < b; ++i) {
        const int ns = v[i] > 0.005f ? 1 : (v[i] < -0.005f ? -1 : 0);
        if (ns != 0) {
            if (sign != 0 && ns != sign) ++crossings;
            sign = ns;
        }
    }
    const double secs = static_cast<double>(b - a) / rate;
    return secs > 0 ? 0.5 * static_cast<double>(crossings) / secs : 0.0;
}

// RMS rozniczy sum(extra) - master na oknie [a, b).
double sumError(const Harness& h, std::size_t a, std::size_t b)
{
    b = std::min({b, h.masterMono.size()});
    double acc = 0;
    for (std::size_t i = a; i < b; ++i) {
        double s = 0;
        for (const auto& e : h.ex) s += i < e.size() ? e[i] : 0.0;
        const double d = s - h.masterMono[i];
        acc += d * d;
    }
    return b > a ? std::sqrt(acc / static_cast<double>(b - a)) : 0.0;
}

} // namespace

// =============================================================================================
TEST_SUITE("engine/channel-scopes")
{
    TEST_CASE("plan: SN76489 daje 4 rozdzielalne kanaly w kolejnosci globalnej")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(phasedTrack())));
        REQUIRE(h.bank.streamCount() == 4);
        CHECK(h.bank.copyCount() == 4);
        for (std::uint16_t c = 0; c < 4; ++c) {
            CHECK(h.bank.desc(c).deviceIdx == 0);
            CHECK(h.bank.desc(c).channelIdx == c);
            CHECK((h.bank.desc(c).flags & kScopeStreamAvailable) != 0);
            CHECK(h.main->channelIsolatable(0, c));
        }
        CHECK_FALSE(h.main->channelIsolatable(0, 4));       // poza zakresem
        CHECK_FALSE(h.main->channelIsolatable(7, 0));       // zly uklad
    }

    TEST_CASE("(a) suma strumieni kanalow ~ master (uklad liniowy), kazdy kanal ma swoja czestotliwosc")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(chordTrack())));
        h.syncAll();
        h.run(kRate);
        REQUIRE(h.ex.size() == 4);
        REQUIRE(h.masterMono.size() >= kRate - 512);
        const double m = rmsOf(h.masterMono, 4000, 40000);
        REQUIRE(m > 0.01);
        // suma kanalow == miks (do bledow zaokraglenia float)
        CHECK(sumError(h, 0, h.masterMono.size()) < 0.01 * m);
        // dominujace czestotliwosci: 440, 880, 1320 Hz; szum wyciszony
        CHECK(freqOf(h.ex[0], 4000, 40000) == doctest::Approx(440.0).epsilon(0.03));
        CHECK(freqOf(h.ex[1], 4000, 40000) == doctest::Approx(880.0).epsilon(0.03));
        CHECK(freqOf(h.ex[2], 4000, 40000) == doctest::Approx(1320.0).epsilon(0.03));
        CHECK(peakOf(h.ex[3], 0, h.ex[3].size()) < 1e-4);
        for (std::size_t s = 0; s < 3; ++s) CHECK(rmsOf(h.ex[s], 4000, 40000) > 0.003);
    }

    TEST_CASE("(b) kanal wyciszony w kopii nie przecieka do innych (fazy: kazdy kanal gra osobno)")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(phasedTrack())));
        h.syncAll();
        h.run(kRate * 2);
        REQUIRE(h.ex.size() == 4);
        const std::size_t half = 24000;      // 0,5 s w klatkach
        // faza k (okno [k*half+3000, (k+1)*half-3000]): gra tylko strumien k
        for (std::size_t phase = 0; phase < 3; ++phase) {
            const std::size_t a = phase * half + 3000, b = (phase + 1) * half - 3000;
            for (std::size_t s = 0; s < 4; ++s) {
                if (s == phase) CHECK(rmsOf(h.ex[s], a, b) > 0.01);
                else CHECK(peakOf(h.ex[s], a, b) < 1e-4);
            }
        }
        // cisza koncowa: wszystkie kopie ciche
        for (std::size_t s = 0; s < 4; ++s) CHECK(peakOf(h.ex[s], 3 * half + 3000, 4 * half - 500) < 1e-4);
    }

    TEST_CASE("(f) MUTE/SOLO na glownym dekoderze nie wplywa na kopie")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(chordTrack())));
        h.syncAll();
        h.main->setMute(0, MuteMask{0xF, 0});        // caly uklad wyciszony na master
        h.run(kRate / 2);
        REQUIRE(h.ex.size() == 4);
        CHECK(peakOf(h.masterMono, 3000, h.masterMono.size()) < 1e-4);
        for (std::size_t s = 0; s < 3; ++s) CHECK(rmsOf(h.ex[s], 3000, 20000) > 0.003);
        // wyciszony w masterze tylko kanal 1: jego strumien nadal gra
        h.main->setMute(0, MuteMask{0x2, 0});
        std::size_t before = h.ex[1].size();
        h.run(kRate / 4);
        CHECK(rmsOf(h.ex[1], before + 1500, h.ex[1].size()) > 0.003);
    }

    TEST_CASE("(c) seek: kopie wracaja do fazy z masterem, pozycje sie zgadzaja")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(phasedTrack())));
        h.syncAll();
        h.run(10000, 512, false);
        // seek w przod do fazy 3 (kanal 2, 1320 Hz)
        h.seek(2 * 24000 + 4000);
        h.ex.clear(); h.masterMono.clear();
        h.run(16000);
        REQUIRE(h.ex.size() == 4);
        CHECK(freqOf(h.ex[2], 1000, 15000) == doctest::Approx(1320.0).epsilon(0.03));
        MESSAGE("peak ex0 [0,15000)=", peakOf(h.ex[0], 0, 15000), " [600,15000)=", peakOf(h.ex[0], 600, 15000));
        CHECK(peakOf(h.ex[0], 600, 15000) < 1e-4);
        CHECK(peakOf(h.ex[1], 600, 15000) < 1e-4);
        CHECK(sumError(h, 0, 15000) < 0.01 * rmsOf(h.masterMono, 1000, 15000));
        CHECK(h.bank.syncedCount() == 4);

        // seek wstecz (Reset + powtorka) do fazy 1 (kanal 0, 440 Hz)
        h.seek(3000);
        h.ex.clear(); h.masterMono.clear();
        h.run(16000);
        CHECK(freqOf(h.ex[0], 1000, 15000) == doctest::Approx(440.0).epsilon(0.03));
        MESSAGE("peak ex2 [0,15000)=", peakOf(h.ex[2], 0, 15000), " [600,15000)=", peakOf(h.ex[2], 600, 15000));
        CHECK(peakOf(h.ex[2], 600, 15000) < 1e-4);
        CHECK(sumError(h, 0, 15000) < 0.01 * rmsOf(h.masterMono, 1000, 15000));
        CHECK(h.bank.syncedCount() == 4);
    }

    TEST_CASE("wlaczenie w polowie utworu: kopie dochodza do pozycji glownego dekodera")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(phasedTrack()), cfg48(), /*bind*/ false));
        h.run(30000, 512, false);                       // bez banku: pozycja ~30000 (faza 2: kanal 1)
        CHECK_FALSE(h.bank.active());
        h.bank.bind(h.track, cfg48(), h.info, *h.main);
        REQUIRE(h.bank.needsSync());
        h.syncAll();
        CHECK(h.bank.syncedCount() == 4);
        h.run(8000);
        CHECK(freqOf(h.ex[1], 500, 7500) == doctest::Approx(880.0).epsilon(0.03));
        // Uwaga: kopie utworzone w polowie utworu startuja ze stanu "po seeku" (rejestry bez przebiegu
        // emulacji), wiec fazy oscylatorow moga sie roznic od mastera - suma NIE musi sie zgadzac.
        CHECK(rmsOf(h.ex[1], 500, 7500) > 0.01);
        CHECK(peakOf(h.ex[0], 500, 7500) < 1e-4);
    }

    TEST_CASE("kopie sa w kroku takze przy blokach roznej wielkosci i dluzszym renderze")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(chordTrack())));
        h.syncAll();
        h.run(7000, 100);
        h.run(7000, 512);
        h.run(7000, 64);
        CHECK(h.bank.syncedCount() == 4);         // zadnych rozjazdow
        CHECK(sumError(h, 0, h.masterMono.size()) < 0.01 * rmsOf(h.masterMono, 2000, 20000));
    }

    TEST_CASE("(d) reset: brak kopii, extraStreamCount = 0")
    {
        Harness h;
        REQUIRE(h.open(makeTrack(chordTrack())));
        h.syncAll();
        h.run(2000);
        CHECK(h.sb->extraStreamCount == 4);
        h.bank.reset();
        CHECK_FALSE(h.bank.active());
        CHECK_FALSE(h.bank.needsSync());
        h.run(2000);
        CHECK(h.sb->extraStreamCount == 0);
    }

    TEST_CASE("kanaly nierozdzielalne: lista zgodna z channelIsolatable na calym korpusie (informacyjnie)")
    {
        const std::filesystem::path dir = testutil::dataDir() / "valid";
        int files = 0;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            auto lt = loader::loadTrackFromFile(e.path());
            if (!lt) continue;
            auto track = std::make_shared<const loader::LoadedTrack>(std::move(lt).value());
            Harness h;
            REQUIRE(h.open(track));
            ++files;
            std::size_t isolatable = 0, total = 0;
            for (const ChannelRef& c : h.info.layout.channels) {
                ++total;
                if (h.main->channelIsolatable(c.deviceIdx, c.channelIdx)) ++isolatable;
            }
            std::size_t flagged = 0;
            for (std::size_t i = 0; i < h.bank.streamCount(); ++i)
                if (h.bank.desc(i).flags & kScopeStreamAvailable) ++flagged;
            CHECK(flagged == std::min<std::size_t>(isolatable, kMaxExtraStreams));
            CHECK(h.bank.streamCount() <= kMaxExtraStreams);
            std::string devs;
            for (const DeviceDesc& d : h.info.layout.devices) devs += d.name + "(" + std::to_string(d.channelCount) + ") ";
            MESSAGE(e.path().filename().string(), ": ", isolatable, "/", total, " rozdzielalnych; ", devs);
        }
        CHECK(files >= 10);
    }

    TEST_CASE("korpus: kopie zostaja zsynchronizowane, a suma ~ master dla ukladow liniowych (informacyjnie)")
    {
        const std::filesystem::path dir = testutil::dataDir() / "valid";
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            auto lt = loader::loadTrackFromFile(e.path());
            if (!lt) continue;
            auto track = std::make_shared<const loader::LoadedTrack>(std::move(lt).value());
            Harness h;
            REQUIRE(h.open(track));
            h.syncAll();
            h.run(kRate * 3);
            const std::size_t copies = h.bank.copyCount();
            CHECK_MESSAGE(h.bank.syncedCount() == copies, e.path().filename().string());
            const double m = rmsOf(h.masterMono, 0, h.masterMono.size());
            const double err = sumError(h, 0, h.masterMono.size());
            MESSAGE(e.path().filename().string(), ": kopie ", copies, ", master rms ", m, ", blad sumy rms ", err,
                    " (", m > 0 ? 100.0 * err / m : 0.0, " %)");
        }
    }
}

// =============================================================================================
namespace {

// Lekka platforma: Engine + PcmRing + watek silnika + konsument udajacy callback audio.
struct ScopeRig {
    PcmRing pcm;
    std::unique_ptr<Engine> eng;
    std::thread th;
    std::uint32_t rate = kRate;
    double speed;
    double owed = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    struct Rec {
        std::uint64_t epoch, startFrame;
        std::uint32_t frames;
        std::uint8_t count;
        double masterRms;
        double err;                         // rms(sum extra - master mono)
        double rmsPerStream[kMaxExtraStreams];
        ScopeStreamDesc desc[kMaxExtraStreams];
    };
    std::vector<Rec> recs;

    explicit ScopeRig(double speedFactor = 1.0, std::uint32_t loops = 2) : speed(speedFactor)
    {
        EngineConfig cfg;
        cfg.decoder.sampleRate = kRate;
        cfg.decoder.loopCount = loops;
        eng = std::make_unique<Engine>(cfg, pcm);
        th = std::thread([this] { eng->run(); });
    }
    ~ScopeRig()
    {
        eng->requestStop();
        th.join();
    }
    void post(Command c)
    {
        while (!eng->post(c)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void run(int ms)
    {
        using clk = std::chrono::steady_clock;
        const auto start = clk::now();
        last = clk::now();
        std::vector<float> buf(2u * 512);
        auto sb = std::make_unique<ScopeBlock>();
        while (clk::now() - start < std::chrono::milliseconds(ms)) {
            const auto now = clk::now();
            owed += std::chrono::duration<double>(now - last).count() * rate * speed;
            last = now;
            while (owed >= 256) { pcm.consume(buf.data(), 256); owed -= 256; }
            while (eng->popScope(*sb)) {
                Rec r{};
                r.epoch = sb->epoch; r.startFrame = sb->startFrame; r.frames = sb->frames; r.count = sb->extraStreamCount;
                double ms2 = 0, es = 0;
                for (std::uint32_t i = 0; i < sb->frames; ++i) {
                    const double m = 0.5 * (sb->masterL[i] + sb->masterR[i]);
                    double s = 0;
                    for (std::uint8_t k = 0; k < sb->extraStreamCount; ++k) s += sb->extra[k][i];
                    ms2 += m * m; es += (s - m) * (s - m);
                }
                if (sb->frames) { r.masterRms = std::sqrt(ms2 / sb->frames); r.err = std::sqrt(es / sb->frames); }
                for (std::uint8_t k = 0; k < sb->extraStreamCount; ++k) {
                    double a = 0;
                    for (std::uint32_t i = 0; i < sb->frames; ++i) a += static_cast<double>(sb->extra[k][i]) * sb->extra[k][i];
                    r.rmsPerStream[k] = sb->frames ? std::sqrt(a / sb->frames) : 0;
                    r.desc[k] = sb->extraDesc[k];
                }
                recs.push_back(r);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    template <class P> bool until(P p, int timeoutMs)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMs)) {
            run(5);
            if (p()) return true;
        }
        return false;
    }
};

} // namespace

TEST_SUITE("engine/channel-scopes-engine")
{
    TEST_CASE("(d) domyslnie wylaczone: extraStreamCount = 0; wlaczenie i wylaczenie przez komende")
    {
        ScopeRig rig(1.0, 0);
        rig.post(cmd::Load{makeTrack(chordTrack(30)), true});
        REQUIRE(rig.until([&] { return rig.recs.size() > 20; }, 4000));
        for (const auto& r : rig.recs) CHECK(r.count == 0);

        // wlaczenie w trakcie: kopie startuja ze stanu "po seeku" (fazy tonow moga sie roznic od mastera),
        // wiec sprawdzamy obecnosc przebiegow, nie rownosc sum
        rig.post(cmd::SetChannelScopes{true});
        rig.recs.clear();
        REQUIRE(rig.until([&] { return !rig.recs.empty() && rig.recs.back().count == 4; }, 4000));
        rig.recs.clear();
        REQUIRE(rig.until([&] { return rig.recs.size() > 60; }, 4000));
        const auto& last = rig.recs.back();
        CHECK(last.count == 4);
        CHECK(last.rmsPerStream[0] > 0.001);
        CHECK(last.rmsPerStream[1] > 0.001);
        CHECK(last.rmsPerStream[2] > 0.001);
        CHECK(last.rmsPerStream[3] < 1e-4);

        rig.post(cmd::SetChannelScopes{false});
        REQUIRE(rig.until([&] { return !rig.recs.empty() && rig.recs.back().count == 0; }, 4000));
    }

    TEST_CASE("(c) Seek i Load: nowa epoka, kopie w fazie z masterem, ciaglosc startFrame")
    {
        ScopeRig rig(1.0, 0);
        rig.post(cmd::SetChannelScopes{true});
        rig.post(cmd::Load{makeTrack(phasedTrack()), true});
        REQUIRE(rig.until([&] { return rig.recs.size() > 40; }, 4000));
        // zaraz po Load (przed pierwszym blokiem) kopie sa utworzone na poz. 0: pierwsze bloki juz zsynchronizowane
        const auto& first = rig.recs.front();
        CHECK(first.count == 4);
        CHECK(first.startFrame == 0);

        const std::uint64_t e0 = rig.eng->snapshot().epoch;
        rig.post(cmd::Seek{2 * 24000 + 6000});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().epoch > e0; }, 3000));
        const std::uint64_t e1 = rig.eng->snapshot().epoch;
        rig.recs.erase(std::remove_if(rig.recs.begin(), rig.recs.end(), [&](const auto& r) { return r.epoch != e1; }), rig.recs.end());
        REQUIRE(rig.until([&] { return rig.recs.size() > 40; }, 4000));
        // ciaglosc startFrame w obrebie epoki; od ustalenia sie kopii (kilka blokow) - suma ~ master
        std::size_t good = 0;
        for (std::size_t i = 1; i < rig.recs.size(); ++i) {
            if (rig.recs[i].epoch != e1) continue;
            CHECK(rig.recs[i].startFrame == rig.recs[i - 1].startFrame + rig.recs[i - 1].frames);
            if (rig.recs[i].count == 4 && rig.recs[i].masterRms > 0.01 && rig.recs[i].err < 0.02 * rig.recs[i].masterRms) ++good;
        }
        CHECK(good > 10);
        // w fazie 3 (do klatki 72000) gra kanal 2, pozostale milcza
        for (const auto& r : rig.recs) {
            if (r.startFrame < 56000 || r.startFrame + r.frames > 70000) continue;
            CHECK(r.rmsPerStream[2] > 0.005);
            CHECK(r.rmsPerStream[0] < 1e-4);
        }

        // Load nowego utworu: nowa epoka, strumienie od poczatku, wlaczone ustawienie przetrwalo Load
        rig.post(cmd::Load{makeTrack(chordTrack()), true});
        REQUIRE(rig.until([&] { return rig.eng->snapshot().epoch > e1; }, 3000));
        const std::uint64_t e2 = rig.eng->snapshot().epoch;
        rig.recs.erase(std::remove_if(rig.recs.begin(), rig.recs.end(), [&](const auto& r) { return r.epoch != e2; }), rig.recs.end());
        REQUIRE(rig.until([&] { return rig.recs.size() > 30; }, 4000));
        CHECK(rig.recs.front().count == 4);
        CHECK(rig.recs.back().err < 0.02 * rig.recs.back().masterRms + 1e-6);
    }

    TEST_CASE("(f) MUTE/SOLO na glownym miksie przez Engine: strumienie kanalow dalej graja")
    {
        ScopeRig rig(1.0, 0);
        rig.post(cmd::SetChannelScopes{true});
        rig.post(cmd::Load{makeTrack(chordTrack(60)), true});
        REQUIRE(rig.until([&] { return rig.recs.size() > 30; }, 4000));
        rig.post(cmd::SetMute{0, MuteMask{0xF, 0}});
        rig.run(400);
        rig.recs.clear();
        REQUIRE(rig.until([&] { return rig.recs.size() > 20; }, 4000));
        const auto& r = rig.recs.back();
        CHECK(r.masterRms < 1e-4);                  // master wyciszony
        CHECK(r.rmsPerStream[0] > 0.001);           // strumienie kanalow nie
        CHECK(r.rmsPerStream[1] > 0.001);
        CHECK(r.rmsPerStream[2] > 0.001);
        // solo na kanale 1: master tylko kanal 1, strumienie wszystkie
        rig.post(cmd::SetMute{0, MuteMask{}});
        rig.post(cmd::SetSolo{0, 1, true});
        rig.run(400);
        rig.recs.clear();
        REQUIRE(rig.until([&] { return rig.recs.size() > 20; }, 4000));
        const auto& s = rig.recs.back();
        CHECK(s.masterRms > 0.001);
        CHECK(s.rmsPerStream[0] > 0.001);
        CHECK(s.rmsPerStream[2] > 0.001);
        CHECK(s.err > 0.001);                       // suma strumieni != master (master = tylko kanal 1)
    }

    TEST_CASE("reconfigure (SetLoopCount) odbudowuje kopie bez utraty strumieni")
    {
        // wzorzec: ta sama sekwencja bez kopii (czy underrun pochodzi z reconfigure, a nie z kopii)
        std::uint32_t baseline = 0;
        {
            ScopeRig rig(1.0, 0);
            rig.post(cmd::Load{makeTrack(chordTrack(60)), true});
            REQUIRE(rig.until([&] { return rig.recs.size() > 20; }, 4000));
            rig.post(cmd::SetLoopCount{3});
            rig.run(300);
            baseline = rig.eng->snapshot().underruns;
        }
        ScopeRig rig(1.0, 0);
        rig.post(cmd::SetChannelScopes{true});
        rig.post(cmd::Load{makeTrack(chordTrack(60)), true});
        REQUIRE(rig.until([&] { return rig.recs.size() > 20; }, 4000));
        const std::uint32_t afterLoad = rig.eng->snapshot().underruns;
        rig.post(cmd::SetLoopCount{3});
        rig.run(300);
        MESSAGE("underruns po Load ", afterLoad, ", po reconfigure ", rig.eng->snapshot().underruns);
        rig.recs.clear();
        REQUIRE(rig.until([&] { return rig.recs.size() > 40; }, 4000));
        const auto& r = rig.recs.back();
        CHECK(r.count == 4);
        CHECK(r.err < 0.02 * r.masterRms + 1e-6);
        MESSAGE("underruns: baseline bez kopii ", baseline, ", z kopiami ", rig.eng->snapshot().underruns);
        // Underrun przy reconfigure zdarza sie takze bez kopii (pierscien jest oprozniany przy nowej epoce, a
        // konsument testowy nie ma buforu urzadzenia) - nie asercja, tylko informacja.
    }
}

// Pomiar kosztu CPU kopii kanalowych: VGMPLAY_SCOPE_BENCH=<plik .vgm/.vgz> (offline, bez watkow).
TEST_SUITE("engine/channel-scopes-bench")
{
    TEST_CASE("koszt CPU kopii kanalowych (VGMPLAY_SCOPE_BENCH)")
    {
        std::string path;
#if defined(_WIN32)
        { char* v = nullptr; size_t len = 0; if (_dupenv_s(&v, &len, "VGMPLAY_SCOPE_BENCH") == 0 && v) path = v; std::free(v); }
#else
        if (const char* v = std::getenv("VGMPLAY_SCOPE_BENCH")) path = v;
#endif
        if (path.empty()) { MESSAGE("pominiete: brak VGMPLAY_SCOPE_BENCH"); return; }
        auto lt = loader::loadTrackFromFile(path);
        REQUIRE(lt);
        auto track = std::make_shared<const loader::LoadedTrack>(std::move(lt).value());
        using clk = std::chrono::steady_clock;
        constexpr std::uint64_t kFrames = kRate * 20;

        auto timeRun = [&](bool withBank, std::uint32_t rate, double& createMs, std::size_t& copies) {
            Harness h;
            DecoderConfig c = cfg48();
            c.sampleRate = rate;
            REQUIRE(h.open(track, c, withBank));
            auto t0 = clk::now();
            if (withBank) h.syncAll();
            createMs = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            copies = h.bank.copyCount();
            t0 = clk::now();
            h.run(kFrames * rate / kRate, 512, false);
            return std::chrono::duration<double>(clk::now() - t0).count();
        };
        double c0, c1; std::size_t n0, n1;
        const double base = timeRun(false, kRate, c0, n0);
        const double with = timeRun(true, kRate, c1, n1);
        MESSAGE("plik: ", path);
        MESSAGE("render 20 s @48 kHz: master ", base * 1000, " ms (", 20.0 / base, "x RT, ", 100.0 * base / 20.0, " % CPU); ",
                "master + ", n1, " kopii: ", with * 1000, " ms (", 20.0 / with, "x RT, ", 100.0 * with / 20.0,
                " % CPU); tworzenie kopii: ", c1, " ms");
        double c2; std::size_t n2;
        const double half = timeRun(false, kRate / 2, c2, n2);
        MESSAGE("master @24 kHz (czy nizsza czestotliwosc kopii cos daje): ", half * 1000, " ms vs ", base * 1000, " ms @48 kHz");
        // typowe amplitudy strumieni kanalow wzgledem mastera (do wzmocnienia wyswietlania w UI)
        {
            Harness h;
            REQUIRE(h.open(track));
            h.syncAll();
            h.run(kRate * 20);
            const double mp = peakOf(h.masterMono, 0, h.masterMono.size());
            const double mr = rmsOf(h.masterMono, 0, h.masterMono.size());
            double maxPk = 0, sumRms = 0;
            std::string per;
            for (std::size_t i = 0; i < h.ex.size(); ++i) {
                const double pk = peakOf(h.ex[i], 0, h.ex[i].size());
                maxPk = std::max(maxPk, pk);
                sumRms += rmsOf(h.ex[i], 0, h.ex[i].size());
                per += "[" + std::to_string(h.desc[i].deviceIdx) + "/" + std::to_string(h.desc[i].channelIdx) + " pk " +
                       std::to_string(pk).substr(0, 5) + " rms " + std::to_string(rmsOf(h.ex[i], 0, h.ex[i].size())).substr(0, 5) + "] ";
            }
            MESSAGE("amplitudy: master peak ", mp, " rms ", mr, "; strumienie: max peak ", maxPk, ", srednie rms ",
                    h.ex.empty() ? 0.0 : sumRms / h.ex.size(), "; ", per);
        }
        CHECK(n1 > 0);
    }
}
