#include "vgmplay_ui/channel_mix.h"

#include <algorithm>

namespace vgmplay::ui {

void ChannelMix::clear()
{
    devices_.clear();
    channels_.clear();
    mute_.clear();
    solo_.clear();
    mutedCount_ = soloCount_ = 0;
}

void ChannelMix::setLayout(const ChannelLayout& layout)
{
    clear();
    for (const DeviceDesc& d : layout.devices) {
        Dev dev;
        dev.channelCount = d.channelCount;
        dev.mainCount = std::min(d.mainChannelCount, d.channelCount);
        devices_.push_back(dev);
    }
    channels_ = layout.channels;
    mute_.assign(channels_.size(), 0);
    solo_.assign(channels_.size(), 0);
}

ChannelRef ChannelMix::ref(int g) const
{
    const int i = index(g);
    return i < 0 ? ChannelRef{} : channels_[static_cast<std::size_t>(i)];
}

bool ChannelMix::muted(int g) const
{
    const int i = index(g);
    return i >= 0 && mute_[static_cast<std::size_t>(i)] != 0;
}

bool ChannelMix::solo(int g) const
{
    const int i = index(g);
    return i >= 0 && solo_[static_cast<std::size_t>(i)] != 0;
}

bool ChannelMix::toggleMute(int g)
{
    const int i = index(g);
    if (i < 0) return false;
    std::uint8_t& m = mute_[static_cast<std::size_t>(i)];
    m = m ? 0 : 1;
    mutedCount_ += m ? 1 : -1;
    return true;
}

bool ChannelMix::toggleSolo(int g)
{
    const int i = index(g);
    if (i < 0) return false;
    std::uint8_t& s = solo_[static_cast<std::size_t>(i)];
    s = s ? 0 : 1;
    soloCount_ += s ? 1 : -1;
    return true;
}

void ChannelMix::muteAll()
{
    std::fill(mute_.begin(), mute_.end(), std::uint8_t{1});
    mutedCount_ = channelCount();
}

void ChannelMix::unmuteAll()
{
    std::fill(mute_.begin(), mute_.end(), std::uint8_t{0});
    mutedCount_ = 0;
}

void ChannelMix::clearSolo()
{
    std::fill(solo_.begin(), solo_.end(), std::uint8_t{0});
    soloCount_ = 0;
}

MuteMask ChannelMix::muteMask(DeviceId device) const
{
    MuteMask m;
    if (device >= devices_.size()) return m;
    const Dev& d = devices_[device];
    for (std::size_t g = 0; g < channels_.size(); ++g) {
        const ChannelRef& r = channels_[g];
        if (r.deviceIdx != device || !mute_[g]) continue;
        if (r.channelIdx < d.mainCount) {
            if (r.channelIdx < 32) m.main |= 1u << r.channelIdx;
        } else {
            const unsigned bit = static_cast<unsigned>(r.channelIdx - d.mainCount);
            if (bit < 32) m.linked |= 1u << bit;
        }
    }
    return m;
}

} // namespace vgmplay::ui
