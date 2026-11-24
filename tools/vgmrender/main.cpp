// vgmrender - render offline VGM -> WAV przez ten sam IDecoder co silnik odtwarzania.
//
//   vgmrender in.vgm out.wav [--rate 44100] [--loops N] [--fade MS] [--float]
//                            [--max-seconds S] [--list-cores]
//   vgmrender in.vgm --gaps [--gap-thr RMS] [--gap-rows]
//
// --gaps: pomiar luk we wzorcu (chipstate::buildPattern). Dla kazdego kanalu z kopia emulacji (dzwiek
// per kanal, ChannelScopeBank) liczy wiersze, w ktorych RMS > prog (domyslnie 0.002), a kanal nie ma
// aktywnej nuty (nuta trzymana od key-on do key-off jest aktywna; po key-off wybrzmiewanie z
// energia nie rosnaca (<= 1.5x poprzedniego wiersza) albo <= 0.3 szczytu nuty nie jest luka; "scisle" liczy je jako luki). Wyjscie TSV: GAP plik typ instancja kanal grajace luki_scisle luki_z_tolerancja;
// z --gap-rows dodatkowo ROW plik kanal wiersz rms (wiersze luk).
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

#include <cmath>
#include <memory>

#include "vgmplay/channel_scopes.hpp"
#include "vgmplay/chipstate.hpp"
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

// ---- --gaps -------------------------------------------------------------------------------------
int measureGaps(const std::string& in, double thr, bool listRows)
{
    auto track = loader::loadTrackFromFile(std::filesystem::path(in));
    if (!track.ok()) { std::fprintf(stderr, "load failed: %s\n", track.error().message.c_str()); return 1; }
    auto tp = std::make_shared<const loader::LoadedTrack>(std::move(track).value());
    auto pat = chipstate::buildPattern(tp->raw(), tp->header, tp->stream);
    if (!pat.ok()) { std::fprintf(stderr, "buildPattern failed: %s\n", pat.error().message.c_str()); return 1; }
    const chipstate::Pattern& P = pat.value();

    DecoderConfig cfg;
    cfg.sampleRate = 44100;     // 1 klatka = 1 tick VGM
    cfg.loopCount = 1;
    cfg.fadeMs = 0;
    auto dec = engine::makeVgmDecoder();
    auto info = dec->open(tp->raw(), cfg);
    if (!info.ok()) { std::fprintf(stderr, "open failed: %s\n", info.error().message.c_str()); return 1; }
    if (!dec->start().ok()) { std::fprintf(stderr, "start failed\n"); return 1; }
    const auto& chans = info.value().layout.channels;
    if (chans.size() != P.channels.size()) {
        std::fprintf(stderr, "layout/pattern mismatch: %zu vs %zu\n", chans.size(), P.channels.size());
        return 1;
    }
    engine::ChannelScopeBank bank;
    bank.bind(tp, cfg, info.value(), *dec);
    for (int g = 0; bank.needsSync() && g < 1000; ++g) bank.syncStep(0, std::chrono::microseconds(1000000));

    // stream -> indeks kanalu wzorca
    const std::size_t ns = bank.streamCount();
    std::vector<int> chanOf(ns, -1);
    for (std::size_t s = 0; s < ns; ++s) {
        const auto& d = bank.desc(s);
        if (!(d.flags & kScopeStreamAvailable)) continue;
        for (std::size_t c = 0; c < chans.size(); ++c)
            if (chans[c].deviceIdx == d.deviceIdx && chans[c].channelIdx == d.channelIdx) chanOf[s] = static_cast<int>(c);
    }
    const std::uint64_t rows = P.rowCount;
    std::vector<std::vector<double>> sumSq(ns, std::vector<double>(rows, 0.0));
    std::vector<std::uint32_t> cnt(rows, 0);
    auto sb = std::make_unique<ScopeBlock>();
    std::vector<float> buf(2u * kMaxBlockFrames);
    SampleTime pos = 0;
    bool ended = false;
    while (!ended && pos / P.rowTicks < rows) {
        const RenderResult r = dec->render(buf.data(), kMaxBlockFrames, sb.get());
        if (r.frames > 0) {
            bank.render(r.frames, pos, sb.get());
            for (std::uint32_t i = 0; i < r.frames; ++i) {
                const std::uint64_t row = (pos + i) / P.rowTicks;
                if (row >= rows) break;
                ++cnt[row];
                for (std::size_t s = 0; s < ns && s < sb->extraStreamCount; ++s) {
                    const double v = sb->extra[s][i];
                    sumSq[s][row] += v * v;
                }
            }
            pos += r.frames;
        }
        ended = r.ended || r.frames == 0;
    }
    for (std::size_t s = 0; s < ns; ++s) {
        const int c = chanOf[s];
        if (c < 0) continue;
        const auto& pc = P.channels[static_cast<std::size_t>(c)];
        if (!pc.decoded) continue;
        bool active = false, tail = false;
        double prev = 0.0, peak = 0.0;
        std::uint64_t playing = 0, strict = 0, tol = 0;
        for (std::uint64_t row = 0; row < rows; ++row) {
            const chipstate::Cell& cell = P.at(row, static_cast<std::size_t>(c));
            if (cell.note >= 0) { active = true; tail = false; peak = 0.0; }
            const bool noteHere = cell.note >= 0;
            if (cell.note == chipstate::kNoteOff) { active = false; tail = true; }
            const bool isActive = active || noteHere || cell.note == chipstate::kNoteOff;   // wiersz "===": nuta grala do key-off w jego trakcie
            const double rms = cnt[row] ? std::sqrt(sumSq[s][row] / cnt[row]) : 0.0;
            if (isActive) peak = std::max(peak, rms);
            if (rms > thr) {
                ++playing;
                if (!isActive) {
                    ++strict;
                    // Wybrzmiewanie po key-off: energia nie rosnie (>1.5x poprzedniego wiersza = nowy atak).
                    const bool decaying = tail && (rms <= 1.5 * prev + thr || rms <= 0.3 * peak);
                    if (decaying) { /* ogon release - nie luka */ }
                    else {
                        ++tol;
                        tail = false;
                        if (listRows) std::printf("ROW\t%s\t%s\t%llu\t%.4f\n", in.c_str(), pc.name.c_str(),
                                                  static_cast<unsigned long long>(row), rms);
                    }
                }
            } else if (!isActive) {
                tail = false;
            }
            prev = rms;
        }
        std::printf("GAP\t%s\t%u\t%u\t%s\t%llu\t%llu\t%llu\n", in.c_str(), static_cast<unsigned>(pc.type),
                    static_cast<unsigned>(pc.instance), pc.name.c_str(), static_cast<unsigned long long>(playing),
                    static_cast<unsigned long long>(strict), static_cast<unsigned long long>(tol));
    }
    return 0;
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
    bool asFloat = false, gaps = false, gapRows = false;
    double gapThr = 0.002;
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
        else if (a == "--gaps") gaps = true;
        else if (a == "--gap-rows") gapRows = true;
        else if (a == "--gap-thr") gapThr = std::strtod(next("--gap-thr"), nullptr);
        else if (!a.empty() && a[0] == '-') { usage(); return 2; }
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else { usage(); return 2; }
    }
    if (gaps && !in.empty()) return measureGaps(in, gapThr, gapRows);
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
