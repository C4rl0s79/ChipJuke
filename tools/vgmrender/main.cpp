// vgmrender - render offline VGM -> WAV przez ten sam IDecoder co silnik odtwarzania.
//
//   vgmrender in.vgm out.wav [--rate 44100] [--loops N] [--fade MS] [--float]
//                            [--max-seconds S] [--list-cores]
//
// Wyjscie: stereo, 16-bit PCM (domyslnie) lub 32-bit float (--float). Rdzenie wg polityki vgmplay
// (docs/licensing/libvgm-audit.md). --loops 0 = bez konca (wymaga --max-seconds).
//
// Plik jest wczytywany przez loader::loadTrackFromFile (gzip/.vgz, pelna walidacja, limity); dekoder
// dodatkowo waliduje bufor sam (obrona w glebi). Ostrzezenia loadera ida na stderr.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fstream>
#include <string>
#include <vector>

#include "vgmplay/loader.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace {

using namespace vgmplay;

void put16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
void putTag(std::vector<std::uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

bool writeWav(const std::string& path, const std::vector<float>& pcm, std::uint32_t rate, bool asFloat)
{
    const std::uint32_t bytesPer = asFloat ? 4 : 2;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size()) * bytesPer;
    std::vector<std::uint8_t> out;
    out.reserve(44 + dataBytes);
    putTag(out, "RIFF"); put32(out, 36 + dataBytes); putTag(out, "WAVE");
    putTag(out, "fmt "); put32(out, 16);
    put16(out, asFloat ? 3 : 1);                 // 3 = IEEE float, 1 = PCM
    put16(out, 2);
    put32(out, rate);
    put32(out, rate * 2 * bytesPer);
    put16(out, static_cast<std::uint16_t>(2 * bytesPer));
    put16(out, static_cast<std::uint16_t>(bytesPer * 8));
    putTag(out, "data"); put32(out, dataBytes);
    for (float f : pcm) {
        if (asFloat) {
            std::uint32_t u;
            std::memcpy(&u, &f, 4);
            put32(out, u);
        } else {
            float c = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
            const float scaled = c * 32767.0f;
            const std::int32_t s = static_cast<std::int32_t>(scaled + (scaled >= 0 ? 0.5f : -0.5f));
            put16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(s)));
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

const char* errorCodeName(ErrorCode c)
{
    static const char* const names[] = {"?", "InvalidArgument", "Io", "NotFound", "AccessDenied", "TooLarge",
        "DecompressionFailed", "Truncated", "BadSignature", "UnsupportedVersion", "BadHeader", "BadCommandStream",
        "BadDataBlock", "LimitExceeded", "OutOfMemory", "Cancelled", "PathRejected", "ParseError",
        "UnsupportedSchema", "DecoderFailure", "Internal"};
    const auto i = static_cast<unsigned>(c);
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

void usage()
{
    std::fprintf(stderr,
        "usage: vgmrender in.vgm|in.vgz out.wav [--rate 44100] [--loops N] [--fade MS] [--float]\n"
        "                                [--max-seconds S]\n"
        "       vgmrender --list-cores\n");
}

int listCores()
{
    auto dec = engine::makeVgmDecoder();
    for (unsigned t = 1; t < static_cast<unsigned>(ChipType::Count_); ++t) {
        const auto type = static_cast<ChipType>(t);
        const auto cores = dec->availableCores(type);
        if (cores.empty()) continue;
        std::printf("type %u:", t);
        for (const auto& c : cores) std::printf(" %s(0x%08X)", c.name.c_str(), c.coreId);
        std::printf("\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string in, out;
    std::uint32_t rate = 44100, loops = 2, fadeMs = 1000;
    double maxSeconds = 0;
    bool asFloat = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", name); std::exit(2); }
            return argv[++i];
        };
        if (a == "--list-cores") return listCores();
        else if (a == "--rate") rate = static_cast<std::uint32_t>(std::strtoul(next("--rate"), nullptr, 10));
        else if (a == "--loops") loops = static_cast<std::uint32_t>(std::strtoul(next("--loops"), nullptr, 10));
        else if (a == "--fade") fadeMs = static_cast<std::uint32_t>(std::strtoul(next("--fade"), nullptr, 10));
        else if (a == "--max-seconds") maxSeconds = std::strtod(next("--max-seconds"), nullptr);
        else if (a == "--float") asFloat = true;
        else if (!a.empty() && a[0] == '-') { usage(); return 2; }
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else { usage(); return 2; }
    }
    if (in.empty() || out.empty()) { usage(); return 2; }
    if (loops == 0 && maxSeconds <= 0) {
        std::fprintf(stderr, "--loops 0 (infinite) requires --max-seconds\n");
        return 2;
    }

    auto track = loader::loadTrackFromFile(std::filesystem::path(in));
    if (!track.ok()) {
        std::fprintf(stderr, "load failed (%s): %s\n", errorCodeName(track.error().code), track.error().message.c_str());
        return 1;
    }
    for (const auto& w : track.value().warnings) std::fprintf(stderr, "warning: %s\n", w.message.c_str());

    DecoderConfig cfg;
    cfg.sampleRate = rate;
    cfg.loopCount = loops;
    cfg.fadeMs = fadeMs;
    auto dec = engine::makeVgmDecoder();
    auto info = dec->open(track.value().raw(), cfg);
    if (!info.ok()) { std::fprintf(stderr, "open failed: %s\n", info.error().message.c_str()); return 1; }
    if (auto s = dec->start(); !s.ok()) { std::fprintf(stderr, "start failed: %s\n", s.error().message.c_str()); return 1; }

    const std::uint64_t maxFrames = maxSeconds > 0 ? static_cast<std::uint64_t>(maxSeconds * rate) : ~std::uint64_t{0};
    std::vector<float> pcm;
    std::vector<float> block(2u * kMaxBlockFrames);
    const auto t0 = std::chrono::steady_clock::now();
    bool ended = false;
    while (!ended && pcm.size() / 2 < maxFrames) {
        const RenderResult r = dec->render(block.data(), kMaxBlockFrames, nullptr);
        pcm.insert(pcm.end(), block.begin(), block.begin() + 2u * r.frames);
        ended = r.ended;
        if (auto e = dec->takeLastError()) { std::fprintf(stderr, "render error: %s\n", e->message.c_str()); return 1; }
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (maxFrames != ~std::uint64_t{0} && pcm.size() / 2 > maxFrames) pcm.resize(2 * maxFrames);

    if (!writeWav(out, pcm, rate, asFloat)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    const double secs = static_cast<double>(pcm.size() / 2) / rate;
    const double wall = std::chrono::duration<double>(t1 - t0).count();
    std::fprintf(stderr, "rendered %.3f s of audio in %.3f s (x%.1f realtime), %u device(s)\n", secs, wall,
                 wall > 0 ? secs / wall : 0.0, static_cast<unsigned>(info.value().layout.devices.size()));
    return 0;
}
