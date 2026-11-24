#include "vgmplay_ui/channel_scopes.h"

#include <algorithm>
#include <cmath>

namespace vgmplay::ui {
namespace {
constexpr std::uint64_t kMask = ChannelScopeBuffer::kCapacity - 1;
}

// ------------------------------------------------------------------ ChannelScopeBuffer

void ChannelScopeBuffer::reset()
{
    slots_.clear();
    epoch_ = 0;
    lastEnd_ = 0;
}

const ChannelScopeBuffer::Slot* ChannelScopeBuffer::find(DeviceId device, std::uint16_t channel) const
{
    for (const Slot& s : slots_)
        if (s.device == device && s.channel == channel) return &s;
    return nullptr;
}

bool ChannelScopeBuffer::live(DeviceId device, std::uint16_t channel) const
{
    const Slot* s = find(device, channel);
    return s && isLive(*s);
}

std::size_t ChannelScopeBuffer::liveCount() const
{
    std::size_t n = 0;
    for (const Slot& s : slots_) n += isLive(s) ? 1 : 0;
    return n;
}

void ChannelScopeBuffer::push(const ScopeBlock& b)
{
    if (b.frames == 0 || b.frames > kMaxBlockFrames) return;
    if (b.epoch < epoch_) return;
    if (b.epoch > epoch_) {
        slots_.clear();
        epoch_ = b.epoch;
    }
    const std::uint32_t count = std::min<std::uint32_t>(b.extraStreamCount, kMaxExtraStreams);
    for (std::uint32_t i = 0; i < count; ++i) {
        const ScopeStreamDesc& d = b.extraDesc[i];
        Slot* slot = nullptr;
        for (Slot& s : slots_)
            if (s.device == d.deviceIdx && s.channel == d.channelIdx) { slot = &s; break; }
        if (!slot) {
            if (slots_.size() >= kMaxSlots) continue;
            slots_.emplace_back();
            slot = &slots_.back();
            slot->device = d.deviceIdx;
            slot->channel = d.channelIdx;
        }
        if (!(d.flags & kScopeStreamAvailable)) {
            slot->valid = false;
            slot->begin = slot->end = b.startFrame;
            continue;
        }
        if (slot->data.empty()) slot->data.assign(kCapacity, 0.0f);
        if (!slot->valid || slot->end == slot->begin || slot->end != b.startFrame)
            slot->begin = slot->end = b.startFrame; // luka / nowy strumien: historia od nowa
        slot->valid = true;
        for (std::uint32_t f = 0; f < b.frames; ++f)
            slot->data[static_cast<std::size_t>((slot->end + f) & kMask)] = b.extra[i][f];
        slot->end += b.frames;
        if (slot->end - slot->begin > kCapacity) slot->begin = slot->end - kCapacity;
    }
    lastEnd_ = b.startFrame + b.frames;
}

bool ChannelScopeBuffer::extract(DeviceId device, std::uint16_t channel, std::uint64_t audible,
                                 std::size_t count, std::vector<float>& out) const
{
    const Slot* s = find(device, channel);
    if (!s || !isLive(*s) || count == 0 || count > kCapacity / 4 || s->end - s->begin < 2 * count + 1)
        return false;
    auto at = [&](std::uint64_t f) { return s->data[static_cast<std::size_t>(f & kMask)]; };
    std::uint64_t t0 = std::max(audible, s->begin + 1);
    if (t0 + 2 * count > s->end) t0 = s->end - 2 * count; // okno szukania + okno rysowane
    // Srodek zakresu w obszarze szukania - odporny na skladowa stala.
    float lo = at(t0), hi = lo;
    for (std::uint64_t f = t0; f < t0 + count; ++f) {
        lo = std::min(lo, at(f));
        hi = std::max(hi, at(f));
    }
    const float mid = 0.5f * (lo + hi);
    std::uint64_t start = t0;
    if (hi - lo > 1e-5f) {
        for (std::uint64_t f = t0; f < t0 + count; ++f) {
            if (at(f - 1) < mid && at(f) >= mid) { start = f; break; }
        }
    }
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = at(start + i);
    return true;
}

bool ChannelScopeBuffer::peak(DeviceId device, std::uint16_t channel, std::uint64_t audible,
                              std::size_t count, float& out) const
{
    const Slot* s = find(device, channel);
    if (!s || !isLive(*s) || count == 0) return false;
    const std::uint64_t avail = s->end - s->begin;
    const std::uint64_t n = std::min<std::uint64_t>(count, avail);
    std::uint64_t t0 = std::max(audible, s->begin);
    if (t0 + n > s->end) t0 = s->end - n;
    float p = 0.0f;
    for (std::uint64_t f = t0; f < t0 + n; ++f)
        p = std::max(p, std::fabs(s->data[static_cast<std::size_t>(f & kMask)]));
    out = p;
    return true;
}

// ------------------------------------------------------------------ VU

float vuLevelFromPeak(float peak)
{
    if (!(peak > 0.0f)) return 0.0f;
    const float db = 20.0f * std::log10(std::min(peak, 1.0f));
    return std::clamp((db + kVuFloorDb) / kVuFloorDb, 0.0f, 1.0f);
}

void VuMeter::update(float target, float dt)
{
    target = std::clamp(target, 0.0f, 1.0f);
    dt = std::clamp(dt, 0.0f, 1.0f);
    if (target >= level) level = target;
    else level = std::max(target, level - kFallPerSec * dt);
    if (level >= peak) {
        peak = level;
        peakAge = 0.0f;
    } else {
        peakAge += dt;
        if (peakAge > kPeakHoldSec) peak = std::max(level, peak - kPeakFallPerSec * dt);
    }
}

int VuMeter::litSegments(float level, int segments)
{
    if (segments <= 0 || !(level > 0.0f)) return 0;
    return std::clamp(static_cast<int>(std::ceil(level * static_cast<float>(segments))), 1, segments);
}

void VuBank::resize(std::size_t channels)
{
    if (channels_.size() != channels) channels_.assign(channels, VuMeter{});
}

void VuBank::reset()
{
    for (VuMeter& m : channels_) m = VuMeter{};
    master_[0] = master_[1] = VuMeter{};
}

void VuBank::update(const std::vector<float>& targets, const float master[2], float dt)
{
    for (std::size_t i = 0; i < channels_.size(); ++i)
        channels_[i].update(i < targets.size() ? targets[i] : 0.0f, dt);
    master_[0].update(master[0], dt);
    master_[1].update(master[1], dt);
}

bool VuBank::active() const
{
    for (const VuMeter& m : channels_)
        if (m.active()) return true;
    return master_[0].active() || master_[1].active();
}

// ------------------------------------------------------------------ siatka

GridLayout chooseScopeGrid(int count, float w, float h, float gap)
{
    GridLayout best;
    best.gap = gap;
    if (count <= 0 || w <= 0.0f || h <= 0.0f) return best;
    float bestScore = -1.0f;
    for (int cols = 1; cols <= count; ++cols) {
        const int rows = (count + cols - 1) / cols;
        const float cw = (w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const float ch = (h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
        if (cw <= 0.0f || ch <= 0.0f) continue;
        // Uzyteczna powierzchnia: proporcje poza 1:1..4:1 sa "marnowane".
        const float score = std::min(cw, 4.0f * ch) * std::min(ch, cw);
        if (score > bestScore + 1e-3f) {
            bestScore = score;
            best.cols = cols;
            best.rows = rows;
            best.cellW = std::floor(cw);
            best.cellH = std::floor(ch);
        }
    }
    if (best.cols == 0) { // obszar mniejszy niz odstepy: pojedyncza kolumna minimalnych komorek
        best.cols = 1;
        best.rows = count;
        best.cellW = std::max(1.0f, std::floor(w));
        best.cellH = std::max(1.0f, std::floor((h - gap * static_cast<float>(count - 1)) / static_cast<float>(count)));
    }
    return best;
}

int moveGridCursor(int cur, int dx, int dy, int cols, int count)
{
    if (count <= 0) return 0;
    cols = std::max(cols, 1);
    cur = std::clamp(cur, 0, count - 1);
    if (dx != 0) return std::clamp(cur + dx, 0, count - 1);
    if (dy != 0) {
        const int next = cur + dy * cols;
        if (next < 0 || next >= count) return cur; // nie wychodzimy poza siatke w pionie
        return next;
    }
    return cur;
}

} // namespace vgmplay::ui
