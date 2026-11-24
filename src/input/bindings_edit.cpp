#include "vgmplay_input/bindings_edit.hpp"

#include <algorithm>
#include <array>

namespace vgmplay::input {
namespace {

constexpr std::array<Action, 8> kCritical = {Action::Up, Action::Down, Action::Left, Action::Right,
                                             Action::Confirm, Action::Back, Action::NextTab, Action::PrevTab};

constexpr std::array<Action, 38> kRebindable = {
    Action::Up, Action::Down, Action::Left, Action::Right, Action::PageUp, Action::PageDown,
    Action::Confirm, Action::Back, Action::Menu, Action::NextTab, Action::PrevTab,
    Action::PlayPause, Action::Stop, Action::PrevTrack, Action::NextTrack, Action::SeekBack, Action::SeekForward,
    Action::VolumeUp, Action::VolumeDown, Action::ToggleLoop, Action::Shuffle,
    Action::ToggleMute, Action::ToggleSolo, Action::UnmuteAll, Action::ClearSolo, Action::ResetMix,
    Action::ToggleTrackerView, Action::PatternFaster, Action::PatternSlower,
    Action::Delete, Action::MoveUp, Action::MoveDown,
    Action::NewPlaylist, Action::RenamePlaylist, Action::DeletePlaylist,
    Action::ToggleSettings, Action::ToggleLang, Action::ToggleGd3Lang,
};

bool hasPad(std::span<const Binding> b, Action a)
{
    return std::any_of(b.begin(), b.end(), [&](const Binding& x) {
        return x.action == a && classOf(x.source.kind) == SourceClass::Gamepad;
    });
}

} // namespace

SourceClass classOf(SourceKind k) noexcept
{
    return k == SourceKind::Keyboard ? SourceClass::Keyboard : SourceClass::Gamepad;
}

bool isCritical(Action a) noexcept
{
    return std::find(kCritical.begin(), kCritical.end(), a) != kCritical.end();
}

std::vector<BindingSource> sourcesOf(std::span<const Binding> b, Action a, SourceClass cls)
{
    std::vector<BindingSource> r;
    for (const Binding& x : b)
        if (x.action == a && classOf(x.source.kind) == cls) r.push_back(x.source);
    return r;
}

std::vector<Action> missingCritical(std::span<const Binding> b)
{
    std::vector<Action> r;
    for (Action a : kCritical)
        if (!hasPad(b, a)) r.push_back(a);
    return r;
}

std::optional<Action> findConflict(std::span<const Binding> b, const BindingSource& src, Action except)
{
    for (const Binding& x : b)
        if (x.source == src && x.action != except && x.action != Action::None) return x.action;
    return std::nullopt;
}

std::vector<Binding> effectiveBindings(std::span<const Binding> stored)
{
    if (stored.empty()) return defaultBindings();
    std::vector<Binding> r(stored.begin(), stored.end());
    const std::vector<Binding> def = defaultBindings();
    for (Action a : missingCritical(r))
        for (const Binding& d : def)
            if (d.action == a && classOf(d.source.kind) == SourceClass::Gamepad) r.push_back(d);
    return r;
}

AssignResult assign(std::vector<Binding>& b, Action a, const BindingSource& src)
{
    AssignResult res;
    if (!actionRebindable(a)) return res;
    const SourceClass cls = classOf(src.kind);
    const std::vector<BindingSource> old = sourcesOf(b, a, cls);
    if (old.size() == 1 && old[0] == src) { res.ok = true; return res; }

    std::vector<Binding> n = b;
    std::vector<Action> losers;
    for (const Binding& x : n)
        if (x.source == src && x.action != a && std::find(losers.begin(), losers.end(), x.action) == losers.end())
            losers.push_back(x.action);
    std::erase_if(n, [&](const Binding& x) {
        return (x.action == a && classOf(x.source.kind) == cls) || (x.source == src && x.action != a);
    });
    n.push_back({a, src});
    if (!old.empty())
        for (Action l : losers) n.push_back({l, old.front()});   // zamiana: przegrany dostaje dawne zrodlo
    if (!losers.empty()) res.swappedWith = losers.front();

    // Zmiana nie moze pogorszyc minimalnego zestawu: nowe braki krytyczne -> odrzucamy.
    const std::vector<Action> before = missingCritical(b);
    for (Action m : missingCritical(n))
        if (std::find(before.begin(), before.end(), m) == before.end()) {
            res.swappedWith.reset();
            return res;
        }
    b = std::move(n);
    res.ok = true;
    return res;
}

bool clearClass(std::vector<Binding>& b, Action a, SourceClass cls)
{
    std::vector<Binding> n = b;
    std::erase_if(n, [&](const Binding& x) { return x.action == a && classOf(x.source.kind) == cls; });
    if (cls == SourceClass::Gamepad && isCritical(a) && !hasPad(n, a)) return false;
    b = std::move(n);
    return true;
}

std::span<const Action> rebindableActions() noexcept { return kRebindable; }

} // namespace vgmplay::input
