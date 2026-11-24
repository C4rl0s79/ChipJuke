#include "vgmplay_ui/pattern_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vgmplay::ui {

std::uint32_t nearestRowTicks(std::uint32_t ticks)
{
    if (ticks == 0) return kDefaultRowTicks;
    std::uint32_t best = kRowTickPresets[0];
    std::uint64_t bestDiff = ~0ull;
    for (const std::uint32_t p : kRowTickPresets) {
        const std::uint64_t d = p > ticks ? p - ticks : ticks - p;
        if (d < bestDiff) {
            bestDiff = d;
            best = p;
        }
    }
    return best;
}

std::uint32_t stepRowTicks(std::uint32_t current, int dir)
{
    const std::uint32_t near = nearestRowTicks(current);
    int idx = 0;
    for (int i = 0; i < kRowTickPresetCount; ++i)
        if (kRowTickPresets[i] == near) idx = i;
    if (dir > 0) idx -= 1;
    else if (dir < 0) idx += 1;
    idx = std::clamp(idx, 0, kRowTickPresetCount - 1);
    return kRowTickPresets[idx];
}

std::string rowTicksLabel(std::uint32_t ticks)
{
    if (ticks == 0) ticks = kDefaultRowTicks;
    const long rate = std::lround(static_cast<double>(kVgmTickRate) / static_cast<double>(ticks));
    char b[24];
    std::snprintf(b, sizeof b, "1/%ld s", std::max(1L, rate));
    return b;
}

std::uint64_t framesToTicks(std::uint64_t frames, std::uint32_t sampleRate)
{
    if (sampleRate == 0) return 0;
    return (frames / sampleRate) * kVgmTickRate + (frames % sampleRate) * kVgmTickRate / sampleRate;
}

PatternTiming timingOf(const chipstate::Pattern& p, bool hasLoop)
{
    PatternTiming t;
    t.rowTicks = p.rowTicks == 0 ? kDefaultRowTicks : p.rowTicks;
    t.rowCount = p.rowCount;
    t.loopStartRow = p.loopStartRow;
    t.hasLoop = hasLoop;
    return t;
}

std::uint64_t rowAtFrame(std::uint64_t audibleFrame, std::uint32_t sampleRate, const PatternTiming& t)
{
    if (t.rowCount == 0 || t.rowTicks == 0) return 0;
    const std::uint64_t row = framesToTicks(audibleFrame, sampleRate) / t.rowTicks;
    if (row < t.rowCount) return row;
    if (t.hasLoop && t.loopStartRow < t.rowCount) {
        const std::uint64_t loopRows = t.rowCount - t.loopStartRow;
        return t.loopStartRow + (row - t.rowCount) % loopRows;
    }
    return t.rowCount - 1;
}

std::optional<std::uint64_t> displayRow(std::int64_t virtualRow, const PatternTiming& t)
{
    if (virtualRow < 0 || t.rowCount == 0) return std::nullopt;
    const std::uint64_t v = static_cast<std::uint64_t>(virtualRow);
    if (v < t.rowCount) return v;
    if (t.hasLoop && t.loopStartRow < t.rowCount)
        return t.loopStartRow + (v - t.rowCount) % (t.rowCount - t.loopStartRow);
    return std::nullopt;
}

std::vector<int> mapPatternColumns(const ChannelLayout& layout, const chipstate::Pattern* pattern)
{
    std::vector<int> out(layout.channels.size(), -1);
    if (!pattern) return out;
    for (std::size_t g = 0; g < layout.channels.size(); ++g) {
        const ChannelRef& r = layout.channels[g];
        if (r.deviceIdx >= layout.devices.size()) continue;
        const DeviceDesc& d = layout.devices[r.deviceIdx];
        for (std::size_t c = 0; c < pattern->channels.size(); ++c) {
            const chipstate::PatternChannel& pc = pattern->channels[c];
            if (pc.type == d.type && pc.instance == d.instance && pc.channelIdx == r.channelIdx) {
                if (pc.decoded) out[g] = static_cast<int>(c);
                break;
            }
        }
    }
    return out;
}

void formatNote(std::int16_t note, char out[4])
{
    static const char* const kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                           "F#", "G-", "G#", "A-", "A#", "B-"};
    if (note == chipstate::kNoteNone) {
        std::snprintf(out, 4, "---");
    } else if (note == chipstate::kNoteOff) {
        std::snprintf(out, 4, "===");
    } else if (note < 0 || note > 127) {
        std::snprintf(out, 4, "???");
    } else {
        const int oct = note / 12 - 1;   // MIDI 60 = C-4
        // Oktawa -1 (MIDI 0..11, np. subbas FM): "_" zamiast cyfry; "-" dawalo "C--"/"C#-" (mylace z "---" i separatorem).
        if (oct < 0) std::snprintf(out, 4, "%s_", kNames[note % 12]);
        else std::snprintf(out, 4, "%s%d", kNames[note % 12], oct);
    }
}

namespace {
char effectChar(std::uint8_t e)
{
    if (e < 10) return static_cast<char>('0' + e);
    if (e < 36) return static_cast<char>('A' + (e - 10));
    return '?';
}
} // namespace

CellText formatCell(const chipstate::Cell& c)
{
    CellText t{};
    formatNote(c.note, t.note);
    if (c.instrument == chipstate::kValueNone) std::snprintf(t.inst, sizeof t.inst, "..");
    else std::snprintf(t.inst, sizeof t.inst, "%02X", static_cast<unsigned>(c.instrument) & 0xFFu);
    if (c.volume == chipstate::kValueNone) std::snprintf(t.vol, sizeof t.vol, "..");
    else std::snprintf(t.vol, sizeof t.vol, "%02X", static_cast<unsigned>(c.volume) & 0xFFu);
    if (c.effect == 0 && c.effectParam == 0) std::snprintf(t.fx, sizeof t.fx, "...");
    else std::snprintf(t.fx, sizeof t.fx, "%c%02X", effectChar(c.effect), static_cast<unsigned>(c.effectParam) & 0xFFu);
    return t;
}

bool cellHasEvent(const chipstate::Cell& c)
{
    return c.note != chipstate::kNoteNone || c.instrument != chipstate::kValueNone ||
           c.volume != chipstate::kValueNone || c.effect != 0 || c.effectParam != 0;
}

bool patternHasEffects(const chipstate::Pattern& p)
{
    for (const chipstate::Cell& c : p.cells)
        if (c.effect != 0 || c.effectParam != 0) return true;
    return false;
}

std::shared_ptr<chipstate::Pattern> makeFakePattern(const ChannelLayout& layout, std::uint64_t totalTicks,
                                                    std::uint64_t loopStartTicks, bool hasLoop,
                                                    std::uint32_t rowTicks)
{
    using namespace chipstate;
    if (rowTicks == 0) rowTicks = kDefaultRowTicks;
    auto p = std::make_shared<Pattern>();
    p->rowTicks = rowTicks;
    p->rowCount = totalTicks / rowTicks + 1;
    p->loopStartRow = hasLoop ? std::min<std::uint64_t>(loopStartTicks / rowTicks, p->rowCount - 1) : 0;
    const std::size_t nch = layout.channels.size();
    for (std::size_t g = 0; g < nch; ++g) {
        const ChannelRef& r = layout.channels[g];
        PatternChannel pc;
        if (r.deviceIdx < layout.devices.size()) {
            const DeviceDesc& d = layout.devices[r.deviceIdx];
            pc.type = d.type;
            pc.instance = d.instance;
            if (r.channelIdx < d.channelNames.size()) pc.name = d.channelNames[r.channelIdx];
        }
        pc.channelIdx = r.channelIdx;
        pc.decoded = !(g == 5 && nch >= 8);
        p->channels.push_back(std::move(pc));
    }
    p->cells.assign(static_cast<std::size_t>(p->rowCount) * nch, Cell{});
    static const int kScale[7] = {0, 2, 3, 5, 7, 8, 10};
    for (std::size_t g = 0; g < nch; ++g) {
        if (!p->channels[g].decoded) continue;
        const std::uint64_t period = 1102ull * (1 + g % 4) + 220ull * (g % 3);   // ~25..120 ms
        for (std::uint64_t row = 0; row < p->rowCount; ++row) {
            const std::uint64_t a = (row * rowTicks + period - 1) / period;
            const std::uint64_t b = ((row + 1) * rowTicks + period - 1) / period;
            if (b <= a) continue;                                   // zadne zdarzenie w tym wierszu
            const std::uint64_t m = a;                              // numer zdarzenia
            Cell& c = p->cells[static_cast<std::size_t>(row) * nch + g];
            if (m % 8 == 7) {
                c.note = kNoteOff;
            } else {
                const int step = static_cast<int>(m % 7);
                const int octave = 3 + static_cast<int>((g / 2 + m / 7) % 3);
                c.note = static_cast<std::int16_t>(12 * (octave + 1) + kScale[(step + static_cast<int>(g) * 2) % 7]);
                c.instrument = static_cast<std::uint8_t>((g * 3 + m / 8) % 16);
                c.volume = static_cast<std::uint8_t>(0x40 - (m * 5) % 0x20);
            }
            if (m % 16 == 5) {
                c.effect = 4;
                c.effectParam = static_cast<std::uint8_t>(0x30 + g);
            }
        }
    }
    return p;
}

} // namespace vgmplay::ui
