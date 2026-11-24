// vgmplay - audio_out: PcmRing (implementacja). Patrz pcm_ring.hpp.
// consume() dziala w callbacku audio: bez alokacji, blokad, logowania i wywolan systemowych.
#include "vgmplay/pcm_ring.hpp"

#include <algorithm>

namespace vgmplay {

PcmRing::PcmRing() : blocks_(std::make_unique<Block[]>(kBlocks)) {}

PcmRing::Block* PcmRing::acquireWrite() noexcept
{
    const std::uint64_t w = write_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_.load(std::memory_order_acquire);
    if (w - r >= kBlocks) return nullptr;
    return &blocks_[static_cast<std::size_t>(w & kMask)];
}

void PcmRing::publish() noexcept
{
    const std::uint64_t w = write_.load(std::memory_order_relaxed);
    const Block& b = blocks_[static_cast<std::size_t>(w & kMask)];
    queued_.fetch_add(static_cast<std::int64_t>(b.frames), std::memory_order_relaxed);
    write_.store(w + 1, std::memory_order_release);
}

void PcmRing::beginEpoch(std::uint64_t epoch) noexcept
{
    epoch_.store(epoch, std::memory_order_release);
}

PcmRing::Audible PcmRing::audible() const noexcept
{
    const std::uint64_t v = audible_.load(std::memory_order_relaxed);
    Audible a;
    a.valid = v != 0;
    a.epoch24 = v >> kAudibleFrameBits;
    a.frame = v & ((std::uint64_t{1} << kAudibleFrameBits) - 1);
    return a;
}

void PcmRing::consume(float* out, std::uint32_t frames) noexcept
{
    const std::uint64_t epoch = epoch_.load(std::memory_order_acquire);
    const float targetVol = volume_.load(std::memory_order_relaxed);
    const bool active = active_.load(std::memory_order_relaxed);
    const float volStep = frames ? (targetVol - curVol_) / static_cast<float>(frames) : 0.0f;
    float vol = curVol_;
    constexpr float kInvDeclick = 1.0f / static_cast<float>(kDeclickFrames);

    bool underran = false;
    bool playedAny = false;
    std::uint64_t audEpoch = 0;
    std::uint64_t audFrame = 0;

    std::uint32_t i = 0;
    while (i < frames) {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        const Block* b = (r != w) ? &blocks_[static_cast<std::size_t>(r & kMask)] : nullptr;

        // Blok z nieaktualnej epoki: odrzuc bez kopiowania; grajaca tresc wycisz zanikiem.
        if (b && b->epoch != epoch) {
            const std::uint32_t left = b->frames - cursor_;
            queued_.fetch_sub(static_cast<std::int64_t>(left), std::memory_order_relaxed);
            consumed_.fetch_add(left, std::memory_order_relaxed);
            cursor_ = 0;
            read_.store(r + 1, std::memory_order_release);
            if (hadData_) { tail_ = kDeclickFrames; hadData_ = false; }
            continue;
        }

        // Zanik ostatniej probki (po przerwaniu tresci): ma pierwszenstwo przed nowymi danymi.
        if (tail_ > 0) {
            const float g = static_cast<float>(tail_) * kInvDeclick;
            out[2 * i + 0] = lastL_ * g * vol;
            out[2 * i + 1] = lastR_ * g * vol;
            --tail_;
            if (tail_ == 0) { lastL_ = 0.0f; lastR_ = 0.0f; }
            vol += volStep;
            ++i;
            continue;
        }

        if (!b) {
            // Pusty pierscien. Jesli grala tresc: zanik zamiast skoku do zera.
            if (hadData_) {
                hadData_ = false;
                if (lastL_ != 0.0f || lastR_ != 0.0f) { tail_ = kDeclickFrames; continue; }
            }
            if (active) underran = true;
            // Reszta bufora: cisza.
            for (; i < frames; ++i) { out[2 * i] = 0.0f; out[2 * i + 1] = 0.0f; }
            vol = targetVol;
            break;
        }

        // Dane: kopiuj, ile sie da, z biezacego bloku.
        if (!hadData_) { hadData_ = true; fadeIn_ = kDeclickFrames; }
        const std::uint32_t n = std::min(b->frames - cursor_, frames - i);
        const float* src = &b->samples[static_cast<std::size_t>(cursor_) * 2];
        for (std::uint32_t k = 0; k < n; ++k) {
            float l = src[2 * k + 0];
            float rr = src[2 * k + 1];
            lastL_ = l;
            lastR_ = rr;
            if (fadeIn_ > 0) {
                const float g = 1.0f - static_cast<float>(fadeIn_) * kInvDeclick;
                l *= g;
                rr *= g;
                --fadeIn_;
            }
            out[2 * (i + k) + 0] = l * vol;
            out[2 * (i + k) + 1] = rr * vol;
            vol += volStep;
        }
        i += n;
        cursor_ += n;
        queued_.fetch_sub(static_cast<std::int64_t>(n), std::memory_order_relaxed);
        consumed_.fetch_add(n, std::memory_order_relaxed);
        playedAny = true;
        audEpoch = b->epoch;
        audFrame = b->startFrame + cursor_;
        if (cursor_ >= b->frames) {
            cursor_ = 0;
            read_.store(r + 1, std::memory_order_release);
        }
    }

    curVol_ = targetVol;
    if (underran) underruns_.fetch_add(1, std::memory_order_relaxed);
    if (playedAny) {
        const std::uint64_t packed =
            ((audEpoch & 0xFFFFFFull) << kAudibleFrameBits) |
            (audFrame & ((std::uint64_t{1} << kAudibleFrameBits) - 1));
        audible_.store(packed, std::memory_order_relaxed);
    }
}

void PcmRing::discardPending() noexcept
{
    const std::uint64_t w = write_.load(std::memory_order_acquire);
    std::uint64_t r = read_.load(std::memory_order_relaxed);
    while (r != w) {
        const Block& b = blocks_[static_cast<std::size_t>(r & kMask)];
        const std::uint32_t left = b.frames - cursor_;
        queued_.fetch_sub(static_cast<std::int64_t>(left), std::memory_order_relaxed);
        consumed_.fetch_add(left, std::memory_order_relaxed);
        cursor_ = 0;
        ++r;
        read_.store(r, std::memory_order_release);
    }
    cursor_ = 0;
    tail_ = 0;
    fadeIn_ = 0;
    hadData_ = false;
    lastL_ = 0.0f;
    lastR_ = 0.0f;
}

} // namespace vgmplay
