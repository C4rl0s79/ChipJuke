#include "vgmplay_ui/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vgmplay::ui {

std::string formatTime(double seconds)
{
    if (!(seconds > 0.0)) seconds = 0.0;
    if (seconds > 359999.0) seconds = 359999.0;
    const auto total = static_cast<std::uint64_t>(seconds);
    const unsigned s = static_cast<unsigned>(total % 60);
    const unsigned m = static_cast<unsigned>((total / 60) % 60);
    const unsigned h = static_cast<unsigned>(total / 3600);
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof buf, "%u:%02u:%02u", h, m, s);
    else std::snprintf(buf, sizeof buf, "%u:%02u", m, s);
    return buf;
}

std::string formatClock(std::uint32_t hz)
{
    char buf[32];
    if (hz >= 1000000u) std::snprintf(buf, sizeof buf, "%.3f MHz", hz / 1.0e6);
    else if (hz >= 1000u) std::snprintf(buf, sizeof buf, "%.0f kHz", hz / 1.0e3);
    else std::snprintf(buf, sizeof buf, "%u Hz", hz);
    return buf;
}

SeekBar makeSeekBar(std::uint64_t audibleFrame, std::uint64_t totalFrames,
                    std::uint64_t introFrames, std::uint64_t loopFrames,
                    std::uint64_t onePassFrames)
{
    SeekBar b;
    if (totalFrames > 0) {
        b.length = totalFrames;
        b.position = std::min(audibleFrame, totalFrames);
        return b;
    }
    b.infinite = true;
    b.length = onePassFrames;
    if (b.length == 0) return b;
    std::uint64_t p = audibleFrame;
    if (p >= onePassFrames && loopFrames > 0) p = introFrames + (p - introFrames) % loopFrames;
    b.position = std::min(p, b.length);
    return b;
}

std::uint64_t fractionToFrame(double fraction, std::uint64_t length)
{
    if (length == 0) return 0;
    if (!(fraction > 0.0)) return 0;
    if (fraction >= 1.0) return length - 1;
    return std::min<std::uint64_t>(length - 1,
                                   static_cast<std::uint64_t>(fraction * static_cast<double>(length)));
}

std::uint64_t offsetFrame(std::uint64_t position, double seconds, std::uint32_t sampleRate,
                          std::uint64_t length)
{
    if (length == 0) return 0;
    const double delta = seconds * static_cast<double>(sampleRate);
    const double target = static_cast<double>(position) + delta;
    if (!(target > 0.0)) return 0;
    if (target >= static_cast<double>(length - 1)) return length - 1;
    return static_cast<std::uint64_t>(target);
}

std::string sanitizeForDisplay(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto c0 = static_cast<unsigned char>(in[i]);
        if (c0 == 0xE2 && i + 2 < in.size()) {
            const auto c1 = static_cast<unsigned char>(in[i + 1]);
            const auto c2 = static_cast<unsigned char>(in[i + 2]);
            const bool bad = (c1 == 0x80 && ((c2 >= 0x8B && c2 <= 0x8F) || (c2 >= 0xAA && c2 <= 0xAE))) ||
                             (c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9);
            if (bad) { i += 2; continue; }
        }
        out.push_back(in[i]);
    }
    return out;
}

} // namespace vgmplay::ui
