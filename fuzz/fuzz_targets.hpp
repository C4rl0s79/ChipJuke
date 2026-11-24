// Ciala wszystkich celow fuzzingu jako funkcje inline zwracajace bool (false = naruszony niezmiennik).
// Uzywane przez:
//  - fuzz_*.cpp (libFuzzer: LLVMFuzzerTestOneInput wola abort(), gdy funkcja zwroci false),
//  - tests/fuzz_regress_test.cpp (doctest na buildzie MSVC odtwarza korpusy regresyjne fuzz/corpus/<cel>/).
// Zmiana ciala celu zmienia oba uzycia naraz; nie dubluj tej logiki w innych plikach.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_common.hpp"
#include "vgmplay/chipstate.hpp"
#include "vgmplay/decoder.hpp"
#include "vgmplay/engine.hpp"
#include "vgmplay/library.hpp"
#include "vgmplay/loader.hpp"
#include "vgmplay/playlist.hpp"
#include "vgmplay/settings.hpp"
#include "vgmplay/vgm_decoder.hpp"

namespace vgmplay::fuzz {

using Bytes = std::span<const std::uint8_t>;

inline std::string_view asText(Bytes in)
{
    return {reinterpret_cast<const char*>(in.data()), in.size()};
}

// Cel 1: bajty -> decompressIfGzip -> parseVgmHeader -> parseGd3 -> validateCommandStream.
// Niezmienniki: offsety <= fileSize, determinizm.
inline bool runVgmHeader(Bytes in)
{
    const auto lim = smallLimits();
    auto d = format::decompressIfGzip(in, lim);
    if (!d) return true;
    const auto& raw = d.value().bytes;
    auto h = format::parseVgmHeader(raw, lim);
    if (!h) return true;
    const auto& hi = h.value();
    if (hi.dataOffset > hi.dataEnd || hi.dataEnd > hi.fileSize || hi.gd3Offset > hi.fileSize ||
        hi.loopOffset > hi.fileSize)
        return false;
    (void)format::parseGd3(raw, hi, lim);
    auto s1 = format::validateCommandStream(raw, hi, lim);
    auto s2 = format::validateCommandStream(raw, hi, lim);
    return s1.ok() == s2.ok();
}

// Cel 2: parseGd3. Wejscie = chunk GD3 (od "Gd3 "), doklejony za 0x40 B zerowego naglowka; gd3Offset = 0x40.
inline bool runGd3(Bytes in)
{
    const auto lim = smallLimits();
    std::vector<std::uint8_t> file(0x40, 0);
    file.insert(file.end(), in.begin(), in.end());
    format::HeaderInfo h;
    h.gd3Offset = 0x40;
    h.fileSize = file.size();
    h.dataEnd = h.fileSize;
    auto g = format::parseGd3(file, h, lim);
    if (g) {
        const auto& t = g.value();
        for (const std::string* f : {&t.trackEn, &t.trackJp, &t.gameEn, &t.gameJp, &t.systemEn, &t.systemJp,
                                     &t.authorEn, &t.authorJp, &t.releaseDate, &t.converter, &t.notes})
            if (f->size() > lim.maxTagFieldBytes) return false;
    }
    return true;
}

// Cel 3: decompressIfGzip - limit wyjscia musi byc zachowany.
inline bool runGzip(Bytes in)
{
    const auto lim = smallLimits();
    auto d = format::decompressIfGzip(in, lim);
    return !(d && d.value().bytes.size() > lim.maxDecompressedBytes);
}

// Cel 4: validateCommandStream na surowych bajtach (bez gzip), z naglowkiem z parseVgmHeader.
inline bool runStream(Bytes in)
{
    const auto lim = smallLimits();
    auto h = format::parseVgmHeader(in, lim);
    if (!h) return true;
    auto s = format::validateCommandStream(in, h.value(), lim);
    return !(s && s.value().totalSamples > lim.maxTotalSamples);
}

#ifdef VGMPLAY_FUZZ_SKIP_OPEN_BUGS
// Otwarte, juz zgloszone bledy z src/ (patrz fuzz/crashes/README.md): pozwala fuzzerowi szukac DALEJ zamiast
// ponownie trafiac w ten sam crash. Wlaczane tylko opcja CMake VGMPLAY_FUZZ_SKIP_OPEN_BUGS; po poprawce usunac.
inline bool hitsKnownOpenBug(Bytes in)
{
    // Zegar ukladu 1..255 Hz => rate rdzenia libvgm = clock / dzielnik = 0 => dzielenie przez zero w RC_SET_RATIO
    // (GB DMG sameboy_apu.c:2324, Mikey mikey.c:973, WonderSwan ws_audio.c:189, ...). Pre-skan formatu
    // (checkChipConfig w src/format/header.cpp) nie odrzuca zegarow dajacych rate 0.
    static constexpr std::uint16_t kClockOfs[] = {
        0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68,
        0x6C, 0x70, 0x74, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4,
        0xB8, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC};
    if (in.size() < 0x40 || in[0] != 'V' || in[1] != 'g' || in[2] != 'm' || in[3] != ' ') return false;
    for (const std::uint16_t ofs : kClockOfs) {
        if (in.size() < static_cast<std::size_t>(ofs) + 4) break;
        const std::uint32_t clk = (in[ofs] | (in[ofs + 1] << 8) | (in[ofs + 2] << 16) |
                                   (static_cast<std::uint32_t>(in[ofs + 3]) << 24)) & 0x3FFFFFFFu;
        if (clk != 0 && clk < 256) return true;
    }
    return false;
}
#endif

// Cel 5: pelny loadTrackFromMemory + render ~2 s audio przez dekoder (libvgm).
inline bool runDecode(Bytes in)
{
#ifdef VGMPLAY_FUZZ_SKIP_OPEN_BUGS
    if (hitsKnownOpenBug(in)) return true;
#endif
    auto t = loader::loadTrackFromMemory(in, {}, smallLimits());
    if (!t) return true;
    auto dec = engine::makeVgmDecoder();
    DecoderConfig cfg;
    cfg.sampleRate = 44100;
    cfg.loopCount = 1;
    cfg.fadeMs = 100;
    if (!dec->open(t.value().raw(), cfg).ok()) return true;
    if (!dec->start().ok()) return true;
    std::vector<float> buf(2u * kMaxBlockFrames);
    std::uint64_t frames = 0;
    while (frames < 2u * 44100) {
        const auto r = dec->render(buf.data(), kMaxBlockFrames, nullptr);
        frames += r.frames;
        if (r.ended || r.frames == 0) break;
    }
    return true;
}

// Cel 6: settings::fromJson (z i bez zgody na sciezki UNC) + niezmiennik toJson -> fromJson (idempotentny).
inline bool runSettings(Bytes in)
{
    const auto json = asText(in);
    for (const bool allowNet : {false, true}) {
        std::vector<Warning> warnings;
        auto s = settings::fromJson(json, &warnings, allowNet);
        if (!s) continue;
        const std::string out = settings::toJson(s.value());
        auto again = settings::fromJson(out, nullptr, allowNet);
        if (!again) return false;
        if (settings::toJson(again.value()) != out) return false;
    }
    return true;
}

// Cel 7: playlist::loadJson (z i bez zgody na UNC) + niezmiennik saveJson -> loadJson (idempotentny).
inline bool runPlaylistJson(Bytes in)
{
    const auto json = asText(in);
    for (const bool allowNet : {false, true}) {
        playlist::PlaylistLoadOptions opt;
        opt.allowNetworkPaths = allowNet;
        auto m = playlist::loadJson(json, opt);
        if (!m) continue;
        const std::string out = playlist::saveJson(m.value());
        auto again = playlist::loadJson(out, opt);
        if (!again) return false;
        if (playlist::saveJson(again.value()) != out) return false;
    }
    return true;
}

// Cel 8: playlist::importM3u (4 kombinacje opcji; stale katalogi bazowy/dozwolony) + exportM3u wyniku.
inline bool runM3u(Bytes in)
{
    const auto text = asText(in);
    for (unsigned mask = 0; mask < 4; ++mask) {
        playlist::M3uImportOptions opt;
        opt.baseDir = L"C:/vgmplay_fuzz_root/music";
        opt.allowedRoot = L"C:/vgmplay_fuzz_root";
        opt.allowAbsolutePaths = (mask & 1) != 0;
        opt.assumeUtf8 = (mask & 2) != 0;
        opt.maxEntries = 2000;
        auto r = playlist::importM3u(text, opt);
        if (!r) continue;
        playlist::Playlist pl;
        pl.name = "fuzz";
        pl.entries = r.value().entries;
        playlist::M3uExportOptions ex;
        ex.baseDir = opt.baseDir;
        ex.utf8 = (mask & 2) != 0;
        (void)playlist::exportM3u(pl, ex);
    }
    return true;
}

// Cel 9: library::MetaCache::fromJson (z i bez UNC) + niezmiennik toJson -> fromJson (ta sama liczba wpisow).
inline bool runLibraryCache(Bytes in)
{
    const auto json = asText(in);
    for (const bool allowNet : {false, true}) {
        auto c = library::MetaCache::fromJson(json, allowNet);
        if (!c) continue;
        const std::string out = c.value().toJson();
        auto again = library::MetaCache::fromJson(out, allowNet);
        if (!again) return false;
        if (again.value().size() != c.value().size()) return false;
    }
    return true;
}

// Cel 10: chipstate::buildPattern na zwalidowanym strumieniu (loadTrackFromMemory jak w celu 5).
// Rozmiar wiersza wybierany z rozmiaru wejscia; maxRows male (ograniczenie pamieci/czasu).
inline bool runPattern(Bytes in)
{
    auto t = loader::loadTrackFromMemory(in, {}, smallLimits());
    if (!t) return true;
    static constexpr std::uint32_t kRowTicks[] = {2205, 735, 64, 44100};
    chipstate::PatternOptions opt;
    opt.rowTicks = kRowTicks[in.size() % 4];
    opt.maxRows = 20000;
    auto p = chipstate::buildPattern(t.value().raw(), t.value().header, t.value().stream, opt);
    if (!p) return true;
    const auto& pat = p.value();
    return pat.cells.size() == static_cast<std::size_t>(pat.rowCount) * pat.channels.size();
}

} // namespace vgmplay::fuzz
