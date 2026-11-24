// Golden audio: TEST REGRESJI, NIE POPRAWNOSCI. Wzorce w tests/golden/golden.json to SHA-256 surowego PCM
// (int16 LE, stereo, 44100 Hz, petle 2, wyciszenie 1000 ms, domyslne rdzenie vgmplay) wyrenderowanego z plikow
// `valid/` oznaczonych tagiem "golden" w manifescie. Brzmienia syntetycznego korpusu nie byly odsluchane -
// zgodny skrot znaczy tylko "dekoder gra TAK SAMO jak w chwili zapisu wzorca".
//
// Test renderuje przez IDecoder bezposrednio (loader -> makeVgmDecoder -> open/start/render), nie przez proces
// vgmrender; tools/golden/update_golden.py liczy te same skroty z WAV z vgmrender, wiec zgodnosc obu drog jest
// dodatkowo sprawdzana przy kazdej aktualizacji wzorca.
//
// Przy niezgodnosci: raport (liczba ramek, RMS, szczyt, RMS roznicy obwiedni 100 ms wzgledem wzorca) i instrukcja
// aktualizacji. Domyslnie niezgodnosc skrotu = blad. Zmienna VGMPLAY_GOLDEN_TOLERANT=1 dopuszcza przejscie, gdy
// dlugosc jest rowna co do ramki, RMS calosci rozni sie <= 1%, a szczyt <= 1 dB (np. CI z innym kompilatorem).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace {

using nlohmann::json;
using namespace vgmplay;

constexpr std::uint32_t kRate = 44100;
constexpr std::uint32_t kWindowFrames = kRate / 10;   // 100 ms
constexpr std::uint64_t kMaxFrames = std::uint64_t{kRate} * 600;

bool envFlag(const char* name)
{
#if defined(_WIN32)
    char* v = nullptr;
    std::size_t n = 0;
    const bool set = _dupenv_s(&v, &n, name) == 0 && v != nullptr && v[0] == '1';
    std::free(v);
    return set;
#else
    const char* v = std::getenv(name);
    return v && v[0] == '1';
#endif
}

struct Render {
    bool ok = false;
    std::string error;
    std::uint64_t frames = 0;
    std::string sha256;
    double rms = 0;
    double peak = 0;                  // 0..1 (po konwersji do int16)
    float maxAbsFloat = 0;            // przed konwersja
    bool finite = true;
    std::vector<double> windowRms;    // RMS w oknach 100 ms (obwiednia)
};

// Ta sama konwersja float -> int16 co w tools/vgmrender (writeWav): clamp, *32767, zaokraglenie.
std::int16_t toInt16(float f)
{
    const float c = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
    const float scaled = c * 32767.0f;
    const std::int32_t s = static_cast<std::int32_t>(scaled + (scaled >= 0 ? 0.5f : -0.5f));
    return static_cast<std::int16_t>(s);
}

Render renderFile(const std::filesystem::path& file)
{
    Render out;
    auto track = loader::loadTrackFromFile(file);
    if (!track.ok()) { out.error = "loader: " + track.error().message; return out; }
    auto dec = engine::makeVgmDecoder();
    DecoderConfig cfg;
    cfg.sampleRate = kRate;
    cfg.loopCount = 2;
    cfg.fadeMs = 1000;
    auto info = dec->open(track.value().raw(), cfg);
    if (!info.ok()) { out.error = "open: " + info.error().message; return out; }
    if (auto s = dec->start(); !s.ok()) { out.error = "start: " + s.error().message; return out; }

    testutil::Sha256 sha;
    std::vector<float> block(2u * kMaxBlockFrames);
    double sumSq = 0, winSq = 0;
    std::uint32_t winFill = 0;
    bool ended = false;
    while (!ended && out.frames < kMaxFrames) {
        const RenderResult r = dec->render(block.data(), kMaxBlockFrames, nullptr);
        for (std::uint32_t i = 0; i < r.frames; ++i) {
            std::int16_t pair[2];
            for (int ch = 0; ch < 2; ++ch) {
                const float f = block[2 * i + ch];
                if (!std::isfinite(f)) out.finite = false;
                out.maxAbsFloat = std::max(out.maxAbsFloat, std::fabs(f));
                pair[ch] = toInt16(f);
                const double v = pair[ch] / 32768.0;
                sumSq += v * v;
                winSq += v * v;
                out.peak = std::max(out.peak, std::fabs(v));
            }
            // little-endian niezaleznie od hosta
            const std::uint8_t le[4] = {static_cast<std::uint8_t>(pair[0] & 0xFF), static_cast<std::uint8_t>((pair[0] >> 8) & 0xFF),
                                        static_cast<std::uint8_t>(pair[1] & 0xFF), static_cast<std::uint8_t>((pair[1] >> 8) & 0xFF)};
            sha.update(le, 4);
            if (++winFill == kWindowFrames) {
                out.windowRms.push_back(std::sqrt(winSq / (2.0 * kWindowFrames)));
                winSq = 0;
                winFill = 0;
            }
        }
        out.frames += r.frames;
        ended = r.ended;
        if (auto e = dec->takeLastError()) { out.error = "render: " + e->message; return out; }
        if (r.frames == 0 && !r.ended) { out.error = "render returned 0 frames without ending"; return out; }
    }
    if (winFill > 0) out.windowRms.push_back(std::sqrt(winSq / (2.0 * winFill)));
    out.rms = out.frames ? std::sqrt(sumSq / (2.0 * static_cast<double>(out.frames))) : 0.0;
    out.sha256 = sha.finishHex();
    out.ok = ended;
    if (!ended) out.error = "render did not end within the frame cap";
    return out;
}

double db(double a, double b) { return (a > 0 && b > 0) ? 20.0 * std::log10(a / b) : (a == b ? 0.0 : 999.0); }

const json& goldenFile()
{
    static const json g = [] {
        const auto bytes = testutil::readAll(std::filesystem::path(VGMPLAY_TEST_DATA_DIR).parent_path() / "golden" / "golden.json");
        REQUIRE_MESSAGE(!bytes.empty(), "brak tests/golden/golden.json (wygeneruj: python3 -I tools/golden/update_golden.py)");
        return json::parse(bytes.begin(), bytes.end());
    }();
    return g;
}

const char* kUpdateHowTo =
    "Jesli zmiana brzmienia jest zamierzona (np. nowy rdzen, poprawka dekodera), po PRZEGLADZIE i odsluchu zaktualizuj wzorce:\n"
    "  python3 -I tools/golden/update_golden.py --vgmrender <sciezka do vgmrender.exe>\n"
    "i dolacz uzasadnienie do commita (golden to test regresji, nie poprawnosci).";

} // namespace

TEST_CASE("golden: lista wzorcow = pliki z tagiem golden w manifescie")
{
    const auto mbytes = testutil::readAll(std::filesystem::path(VGMPLAY_TEST_DATA_DIR) / "manifest.json");
    const json m = json::parse(mbytes.begin(), mbytes.end());
    std::set<std::string> tagged, golden;
    for (const auto& f : m["files"])
        for (const auto& t : f.value("tags", json::array()))
            if (t == "golden") tagged.insert(f["path"].get<std::string>());
    for (const auto& f : goldenFile()["files"]) golden.insert(f["path"].get<std::string>());
    CHECK_MESSAGE(tagged == golden, "tests/golden/golden.json nie pokrywa sie z tagiem golden w manifescie - " << kUpdateHowTo);
    CHECK(goldenFile()["render"]["sampleRate"].get<std::uint32_t>() == kRate);
    CHECK(goldenFile()["render"]["loops"].get<std::uint32_t>() == 2);
    CHECK(goldenFile()["render"]["fadeMs"].get<std::uint32_t>() == 1000);
}

TEST_CASE("golden: skrot PCM zgodny ze wzorcem (test regresji, nie poprawnosci)")
{
    const bool tolerant = envFlag("VGMPLAY_GOLDEN_TOLERANT");
    for (const auto& g : goldenFile()["files"]) {
        const std::string path = g["path"].get<std::string>();
        SUBCASE(path.c_str())
        {
            const Render r = renderFile(std::filesystem::path(VGMPLAY_TEST_DATA_DIR) / path);
            INFO(r.error);
            REQUIRE(r.ok);

            // Sprawdzenia bez wzorca.
            CHECK_MESSAGE(r.finite, "NaN/Inf w wyjsciu");
            CHECK_MESSAGE(r.maxAbsFloat <= 1.0f, "klipowanie: |x| = " << r.maxAbsFloat);
            CHECK_MESSAGE(r.rms > 0.0, "cisza w calym utworze");
            CHECK(r.frames > 0);

            const std::string refSha = g["sha256"].get<std::string>();
            if (r.sha256 == refSha) {
                CHECK(r.frames == g["frames"].get<std::uint64_t>());
                continue;
            }

            // Niezgodnosc: raport roznic wzgledem wzorca (obwiednia 100 ms).
            const auto refWin = g["windowRms"].get<std::vector<double>>();
            const std::size_t n = std::min(refWin.size(), r.windowRms.size());
            double dsq = 0;
            for (std::size_t i = 0; i < n; ++i) dsq += (r.windowRms[i] - refWin[i]) * (r.windowRms[i] - refWin[i]);
            const double envDiff = n ? std::sqrt(dsq / static_cast<double>(n)) : 0.0;
            const double refRms = g["rms"].get<double>(), refPeak = g["peak"].get<double>();
            const std::uint64_t refFrames = g["frames"].get<std::uint64_t>();
            const double rmsRel = refRms > 0 ? std::fabs(r.rms - refRms) / refRms : 1.0;
            const double peakDb = db(r.peak, refPeak);
            const bool tolOk = r.frames == refFrames && rmsRel <= 0.01 && std::fabs(peakDb) <= 1.0;

            INFO("plik: " << path);
            INFO("ramki: " << r.frames << " (wzorzec " << refFrames << ")");
            INFO("RMS: " << r.rms << " (wzorzec " << refRms << ", roznica wzgledna " << rmsRel * 100 << " %)");
            INFO("szczyt: " << r.peak << " (wzorzec " << refPeak << ", " << peakDb << " dB)");
            INFO("RMS roznicy obwiedni 100 ms: " << envDiff << " (okien porownanych: " << n << ")");
            INFO("sha256: " << r.sha256 << " (wzorzec " << refSha << ")");
            INFO("tolerancja (ramki ==, RMS <= 1%, szczyt <= 1 dB): " << (tolOk ? "SPELNIONA" : "przekroczona"));
            INFO(kUpdateHowTo);
            if (tolerant && tolOk)
                MESSAGE("[GOLDEN] skrot rozny, ale w tolerancji (VGMPLAY_GOLDEN_TOLERANT=1): " << path);
            else
                FAIL_CHECK("skrot PCM rozny od wzorca: " << path);
        }
    }
}
