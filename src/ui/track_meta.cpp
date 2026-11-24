#include "vgmplay_ui/track_meta.h"

#include "vgmplay_ui/format.h"
#include "vgmplay_ui/strings.h"

#include <cwctype>

namespace vgmplay::ui {

std::wstring pathKey(const std::filesystem::path& p)
{
    std::wstring w = p.lexically_normal().generic_wstring();
    for (wchar_t& c : w) c = static_cast<wchar_t>(std::towlower(c));
    return w;
}

std::string entryTitle(const playlist::Entry& e, const TrackMetaStore* meta, format::Gd3Language lang)
{
    if (meta) {
        if (const TrackDisplay* d = meta->find(e.path)) {
            const std::string& a = lang == format::Gd3Language::Jp ? d->trackJp : d->trackEn;
            const std::string& b = lang == format::Gd3Language::Jp ? d->trackEn : d->trackJp;
            if (!a.empty()) return sanitizeForDisplay(a);
            if (!b.empty()) return sanitizeForDisplay(b);
        }
    }
    if (!e.title.empty()) return sanitizeForDisplay(e.title);
    const auto u8 = e.path.filename().u8string();
    std::string name = sanitizeForDisplay(std::string(u8.begin(), u8.end()));
    if (meta) {
        if (const TrackDisplay* d = meta->find(e.path); d && d->cloud) name += std::string("  [") + tr(Str::CloudTag) + "]";
    }
    return name;
}

std::uint64_t entryDurationMs(const playlist::Entry& e, const TrackMetaStore* meta)
{
    if (meta) {
        if (const TrackDisplay* d = meta->find(e.path); d && d->durationMs) return d->durationMs;
    }
    return e.durationMs;
}

} // namespace vgmplay::ui
