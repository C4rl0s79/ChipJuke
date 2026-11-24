// Nazwy akcji i zrodel (serializacja) oraz domyslne wiazania.
#include "vgmplay/input.hpp"

#include <array>
#include <cstddef>

namespace vgmplay::input {
namespace {

// Kolejnosc = kolejnosc enum Action (static_assert pilnuje dlugosci). Nazwy sa czescia formatu pliku.
constexpr std::array<std::string_view, static_cast<std::size_t>(Action::Count_)> kActionNames = {
    "",                  // None
    "up", "down", "left", "right", "pageUp", "pageDown",
    "confirm", "back", "menu",
    "playPause", "stop", "nextTrack", "prevTrack", "seekForward", "seekBack",
    "volumeUp", "volumeDown", "toggleLoop",
    "nextTab", "prevTab", "toggleTrackerView",
    "toggleMute", "toggleSolo",
    "openFile", "quit",
    "toggleLang", "toggleGd3Lang", "toggleDiag",
    "unmuteAll", "clearSolo", "resetMix",
    "patternFaster", "patternSlower",
    "delete", "moveUp", "moveDown",
    "shuffle", "toggleSettings",
    "newPlaylist", "renamePlaylist", "deletePlaylist",
    "muteChannel", "soloChannel",
};

constexpr std::array<std::string_view, 4> kKindNames = {"key", "padButton", "padAxisPositive", "padAxisNegative"};

// SDL3: SDL_Scancode (kody USB HID), SDL_GamepadButton, SDL_GamepadAxis. Zgodnosc sprawdza test (input_test).
namespace key {
constexpr std::int32_t C = 6, M = 16, N = 17, P = 19, R = 21, S = 22, U = 24;
constexpr std::int32_t Return = 40, Escape = 41, Tab = 43, Space = 44, Minus = 45, Equals = 46;
constexpr std::int32_t F2 = 59, F3 = 60, F4 = 61, F6 = 63, F7 = 64, F8 = 65, F9 = 66, F10 = 67, F12 = 69;
constexpr std::int32_t PageUp = 75, Delete = 76, PageDown = 78;
constexpr std::int32_t Right = 79, Left = 80, Down = 81, Up = 82;
constexpr std::int32_t KpMinus = 86, KpPlus = 87, KpEnter = 88;
} // namespace key
namespace pad {
constexpr std::int32_t South = 0, East = 1, West = 2, North = 3, Back = 4, Start = 6;
constexpr std::int32_t LeftStick = 7, RightStick = 8, LeftShoulder = 9, RightShoulder = 10;
constexpr std::int32_t DpadUp = 11, DpadDown = 12, DpadLeft = 13, DpadRight = 14;
constexpr std::int32_t AxisLeftX = 0, AxisLeftY = 1, AxisRightY = 3, AxisLeftTrigger = 4, AxisRightTrigger = 5;
} // namespace pad

} // namespace

std::string_view actionName(Action a) noexcept
{
    const auto i = static_cast<std::size_t>(a);
    return i < kActionNames.size() ? kActionNames[i] : std::string_view{};
}

std::optional<Action> actionFromName(std::string_view name) noexcept
{
    if (name.empty()) return std::nullopt;
    for (std::size_t i = 1; i < kActionNames.size(); ++i)
        if (kActionNames[i] == name) return static_cast<Action>(i);
    return std::nullopt;
}

std::string_view sourceKindName(SourceKind k) noexcept
{
    const auto i = static_cast<std::size_t>(k);
    return i < kKindNames.size() ? kKindNames[i] : std::string_view{};
}

std::optional<SourceKind> sourceKindFromName(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kKindNames.size(); ++i)
        if (kKindNames[i] == name) return static_cast<SourceKind>(i);
    return std::nullopt;
}

bool actionRebindable(Action a) noexcept
{
    return a != Action::None && a < Action::Count_ && a != Action::MuteChannel && a != Action::SoloChannel;
}

std::vector<Binding> defaultBindings()
{
    std::vector<Binding> b;
    auto k = [&](Action a, std::int32_t code) { b.push_back({a, {SourceKind::Keyboard, code}}); };
    auto p = [&](Action a, std::int32_t code) { b.push_back({a, {SourceKind::GamepadButton, code}}); };
    auto ax = [&](Action a, std::int32_t axis, bool positive) {
        b.push_back({a, {positive ? SourceKind::GamepadAxisPositive : SourceKind::GamepadAxisNegative, axis}});
    };

    // Klawiatura: dzisiejszy uklad (Ctrl/Shift + klawisz oraz cyfry kanalow obsluguje warstwa UI lokalnie).
    k(Action::Up, key::Up); k(Action::Down, key::Down); k(Action::Left, key::Left); k(Action::Right, key::Right);
    k(Action::PageUp, key::PageUp); k(Action::PageDown, key::PageDown);
    k(Action::Confirm, key::Return); k(Action::Confirm, key::KpEnter);
    k(Action::Back, key::Escape);
    k(Action::PlayPause, key::Space);
    k(Action::NextTab, key::Tab);                       // Shift+Tab = PrevTab (lokalnie)
    k(Action::ToggleLang, key::F2); k(Action::ToggleGd3Lang, key::F3); k(Action::ToggleTrackerView, key::F4);
    k(Action::PatternFaster, key::Equals); k(Action::PatternFaster, key::KpPlus);
    k(Action::PatternSlower, key::Minus); k(Action::PatternSlower, key::KpMinus);
    k(Action::ToggleMute, key::M); k(Action::ToggleSolo, key::S);
    k(Action::UnmuteAll, key::U); k(Action::ClearSolo, key::C); k(Action::ResetMix, key::R);
    k(Action::Delete, key::Delete);                     // Shift+Delete = DeletePlaylist (lokalnie)
    k(Action::NewPlaylist, key::F8); k(Action::RenamePlaylist, key::F9);
    k(Action::PrevTrack, key::P); k(Action::NextTrack, key::N);
    k(Action::ToggleLoop, key::F6); k(Action::Shuffle, key::F7);
    k(Action::ToggleSettings, key::F10); k(Action::ToggleDiag, key::F12);

    // Pad (uklad Xbox; SDL mapuje inne pady na te same pozycje).
    p(Action::Up, pad::DpadUp); p(Action::Down, pad::DpadDown);
    p(Action::Left, pad::DpadLeft); p(Action::Right, pad::DpadRight);
    ax(Action::Up, pad::AxisLeftY, false); ax(Action::Down, pad::AxisLeftY, true);
    ax(Action::Left, pad::AxisLeftX, false); ax(Action::Right, pad::AxisLeftX, true);
    ax(Action::PageUp, pad::AxisRightY, false); ax(Action::PageDown, pad::AxisRightY, true);
    p(Action::Confirm, pad::South);                     // A
    p(Action::Back, pad::East);                         // B
    p(Action::ToggleSolo, pad::West);                   // X: solo (wzorzec) / usun wpis (playlista)
    p(Action::Menu, pad::North);                        // Y: menu / ustawienia
    p(Action::PlayPause, pad::Start);
    p(Action::ToggleTrackerView, pad::Back);            // Select
    p(Action::NextTab, pad::RightShoulder); p(Action::PrevTab, pad::LeftShoulder);   // RB / LB
    p(Action::PrevTrack, pad::LeftStick); p(Action::NextTrack, pad::RightStick);
    ax(Action::PrevTrack, pad::AxisLeftTrigger, true); ax(Action::NextTrack, pad::AxisRightTrigger, true);   // LT / RT
    return b;
}

} // namespace vgmplay::input
