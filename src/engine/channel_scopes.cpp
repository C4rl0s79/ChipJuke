// vgmplay - engine: ChannelScopeBank (kopie emulacji z maska MUTE). Patrz channel_scopes.hpp.
// WATEK: wylacznie watek silnika.
#include "vgmplay/channel_scopes.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "vgmplay/vgm_decoder.hpp"

namespace vgmplay::engine {

void ChannelScopeBank::reset() noexcept
{
    try {
        for (Stream& s : streams_) {
            if (s.dec) s.dec->close();
            s.dec.reset();
        }
        streams_.clear();
        streams_.shrink_to_fit();
        devices_.clear();
        devices_.shrink_to_fit();
        scratch_.clear();
        scratch_.shrink_to_fit();
    } catch (...) {
    }
    track_.reset();
    deviceCount_ = 0;
}

void ChannelScopeBank::bind(std::shared_ptr<const loader::LoadedTrack> track, const DecoderConfig& cfg,
                            const TrackInfo& info, const IDecoder& main)
{
    reset();
    if (!track || track->raw().empty()) return;
    track_ = std::move(track);
    cfg_ = cfg;
    devices_ = info.layout.devices;
    deviceCount_ = devices_.size();

    // Ile kopii zmiesci sie w budzecie pamieci.
    const std::uint64_t perCopy = 2ull * track_->raw().size() + kPerCopyOverheadBytes;
    const std::size_t memCap = static_cast<std::size_t>(std::min<std::uint64_t>(
        kMemoryBudgetBytes / perCopy, kMaxExtraStreams));

    // Wybor strumieni: rozdzielalne w kolejnosci globalnej do limitu, potem nierozdzielalne (flags == 0).
    std::vector<bool> isolatable(info.layout.channels.size(), false);
    std::vector<bool> chosen(info.layout.channels.size(), false);
    std::size_t taken = 0;
    for (std::size_t i = 0; i < info.layout.channels.size(); ++i) {
        const ChannelRef& c = info.layout.channels[i];
        isolatable[i] = main.channelIsolatable(c.deviceIdx, c.channelIdx);
        if (isolatable[i] && taken < memCap) { chosen[i] = true; ++taken; }
    }
    for (std::size_t i = 0; i < info.layout.channels.size() && taken < kMaxExtraStreams; ++i) {
        if (!isolatable[i] && !chosen[i]) { chosen[i] = true; ++taken; }
    }
    // Brak jakiegokolwiek rozdzielalnego kanalu: nie ma po co listowac samych "niedostepnych".
    if (std::none_of(isolatable.begin(), isolatable.end(), [](bool b) { return b; })) return;

    for (std::size_t i = 0; i < info.layout.channels.size(); ++i) {
        if (!chosen[i]) continue;
        const ChannelRef& c = info.layout.channels[i];
        Stream s;
        s.desc.deviceIdx = c.deviceIdx;
        s.desc.channelIdx = c.channelIdx;
        s.wanted = isolatable[i];
        s.desc.flags = s.wanted ? kScopeStreamAvailable : 0;
        if (s.wanted) {
            const DeviceDesc& d = devices_[c.deviceIdx];
            MuteMask m{0xFFFFFFFFu, 0xFFFFFFFFu};
            if (c.channelIdx < d.mainChannelCount)
                m.main &= ~(1u << c.channelIdx);
            else
                m.linked &= ~(1u << (c.channelIdx - d.mainChannelCount));
            s.mask = m;
        }
        streams_.push_back(std::move(s));
    }
    scratch_.assign(2u * kMaxBlockFrames, 0.0f);
}

void ChannelScopeBank::invalidate() noexcept
{
    for (Stream& s : streams_) { s.synced = false; s.mismatches = 0; }
}

bool ChannelScopeBank::needsSync() const noexcept
{
    return std::any_of(streams_.begin(), streams_.end(),
                       [](const Stream& s) { return s.wanted && !s.synced; });
}

std::size_t ChannelScopeBank::copyCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(streams_.begin(), streams_.end(),
                                                  [](const Stream& s) { return s.wanted; }));
}

std::size_t ChannelScopeBank::syncedCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(streams_.begin(), streams_.end(),
                                                  [](const Stream& s) { return s.wanted && s.synced; }));
}

bool ChannelScopeBank::syncOne(Stream& s, SampleTime pos)
{
    try {
        const bool fresh = !s.dec;
        if (fresh) {
            s.dec = makeVgmDecoder();
            auto opened = s.dec->open(track_->raw(), cfg_);
            if (!opened.ok()) return false;
            // Uklad urzadzen musi byc identyczny jak w glownym dekoderze (te same deterministyczne zasady).
            if (opened.value().layout.devices.size() != deviceCount_) return false;
            if (!s.dec->start().ok()) return false;
            for (std::size_t d = 0; d < deviceCount_; ++d)
                if (d != s.desc.deviceIdx) s.dec->setDeviceFrozen(static_cast<DeviceId>(d), true);
        }
        if (!fresh || pos > 0) {
            if (!s.dec->seek(pos).ok()) return false;
        }
        // Maske ustawiamy po kazdym przewinieciu (Reset w libvgm nie powinien jej tykac, ale to tanie).
        s.dec->setMute(s.desc.deviceIdx, s.mask);
        s.skew = static_cast<std::int64_t>(s.dec->positionFrames()) - static_cast<std::int64_t>(pos);
        if (std::llabs(s.skew) > kMaxSkewFrames) return false;
        s.synced = true;
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t ChannelScopeBank::syncStep(SampleTime pos, std::chrono::microseconds budget)
{
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    std::size_t done = 0;
    for (Stream& s : streams_) {
        if (!s.wanted || s.synced) continue;
        if (done > 0 && clk::now() - t0 >= budget) break;
        if (syncOne(s, pos)) {
            ++done;
        } else {
            // Kopia nie daje sie utrzymac: strumien zostaje na liscie jako niedostepny (cisza).
            s.wanted = false;
            s.synced = false;
            s.desc.flags = 0;
            if (s.dec) { s.dec->close(); s.dec.reset(); }
            ++done;
        }
    }
    return done;
}

void ChannelScopeBank::render(std::uint32_t frames, SampleTime start, ScopeBlock* sb) noexcept
{
    if (streams_.empty()) {
        if (sb) sb->extraStreamCount = 0;
        return;
    }
    frames = std::min(frames, kMaxBlockFrames);
    if (sb) sb->extraStreamCount = static_cast<std::uint8_t>(std::min<std::size_t>(streams_.size(), kMaxExtraStreams));
    for (std::size_t i = 0; i < streams_.size() && i < kMaxExtraStreams; ++i) {
        Stream& s = streams_[i];
        float* row = sb ? sb->extra[i] : nullptr;
        if (sb) sb->extraDesc[i] = s.desc;
        std::uint32_t got = 0;
        if (s.wanted && s.synced && s.dec) {
            const RenderResult r = s.dec->render(scratch_.data(), frames, nullptr);
            got = std::min(r.frames, frames);
            const std::int64_t expectPos = static_cast<std::int64_t>(start) + frames + s.skew;
            if (got != frames || static_cast<std::int64_t>(s.dec->positionFrames()) != expectPos) {
                // rozjazd wzgledem glownego dekodera (np. inny koniec utworu): ponowna synchronizacja
                s.synced = false;
                if (++s.mismatches > kMaxMismatches) { s.wanted = false; s.desc.flags = 0; }
            }
            if (row) {
                for (std::uint32_t k = 0; k < got; ++k)
                    row[k] = 0.5f * (scratch_[2 * k] + scratch_[2 * k + 1]);
            }
        }
        if (row && got < frames) std::memset(row + got, 0, (frames - got) * sizeof(float));
    }
}

} // namespace vgmplay::engine
