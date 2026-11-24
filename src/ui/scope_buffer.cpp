#include "vgmplay_ui/scope_buffer.h"

#include <algorithm>
#include <cmath>

namespace vgmplay::ui {
namespace {
constexpr std::uint64_t kMask = ScopeBuffer::kCapacity - 1;
}

void ScopeBuffer::reset()
{
    epoch_ = 0;
    begin_ = end_ = 0;
}

void ScopeBuffer::push(const ScopeBlock& b)
{
    if (b.frames == 0 || b.frames > kMaxBlockFrames) return;
    if (b.epoch < epoch_) return; // blok z minionej epoki
    if (b.epoch > epoch_ || empty() || b.startFrame != end_) {
        epoch_ = b.epoch;
        begin_ = end_ = b.startFrame;
    }
    for (std::uint32_t i = 0; i < b.frames; ++i) {
        const std::size_t idx = static_cast<std::size_t>((end_ + i) & kMask);
        l_[idx] = b.masterL[i];
        r_[idx] = b.masterR[i];
    }
    end_ += b.frames;
    if (end_ - begin_ > kCapacity) begin_ = end_ - kCapacity;
}

float ScopeBuffer::mono(std::uint64_t frame) const
{
    const std::size_t idx = static_cast<std::size_t>(frame & kMask);
    return l_[idx] + r_[idx];
}

bool ScopeBuffer::extract(std::uint64_t audible, std::size_t count, std::vector<float>& l,
                          std::vector<float>& r, std::uint64_t* startedAt) const
{
    // Potrzebna jest jedna klatka przed oknem (porownanie triggera) i `count` klatek okna.
    if (count == 0 || count > kCapacity / 2 || end_ - begin_ < count + 1) return false;
    std::uint64_t t0 = std::max(audible, begin_ + 1);
    // Gdy pozycja slyszana jest przy samym koncu danych, okno cofamy tak, by sie miescilo.
    if (t0 + count > end_) t0 = end_ - count;
    std::uint64_t start = t0;
    // Szukamy do `count` klatek za t0, ale okno nadal musi sie miescic w danych.
    const std::uint64_t searchEnd = std::min<std::uint64_t>(t0 + count, end_ - count + 1);
    for (std::uint64_t f = t0; f < searchEnd; ++f) {
        if (mono(f - 1) < 0.0f && mono(f) >= 0.0f) {
            start = f;
            break;
        }
    }
    l.resize(count);
    r.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t idx = static_cast<std::size_t>((start + i) & kMask);
        l[i] = l_[idx];
        r[i] = r_[idx];
    }
    if (startedAt) *startedAt = start;
    return true;
}

bool ScopeBuffer::peak(std::uint64_t audible, std::size_t count, float& l, float& r) const
{
    if (count == 0 || end_ == begin_) return false;
    const std::uint64_t n = std::min<std::uint64_t>(count, end_ - begin_);
    std::uint64_t t0 = std::max(audible, begin_);
    if (t0 + n > end_) t0 = end_ - n;
    l = r = 0.0f;
    for (std::uint64_t f = t0; f < t0 + n; ++f) {
        const std::size_t idx = static_cast<std::size_t>(f & kMask);
        l = std::max(l, std::fabs(l_[idx]));
        r = std::max(r, std::fabs(r_[idx]));
    }
    return true;
}

} // namespace vgmplay::ui
