// vgmplay - kontrakt: input.hpp
// Akcje, ekrany i router wejscia (klawiatura + pad -> ActionEvent). Bez SDL/ImGui:
// warstwa app/ui tlumaczy zdarzenia SDL na RawInput.
//
// WATEK: tylko watek UI/petli zdarzen (brak synchronizacji).
//
// ZASADY (kontrakt, wiazace dla implementacji i testow):
//  1. Wlasnosc nacisniecia. Nacisniecie nalezy do ekranu, na ktorym je ROZPOCZETO
//     (ActionEvent::screen = ekran z chwili wcisniecia). Wszystkie zdarzenia tego samego
//     nacisniecia (Pressed, Repeat, Released) niosa ten sam screen.
//  2. Zmiana ekranu (setContext). Przycisk wciśniety w chwili zmiany jest "zatrzasniety":
//     nowy ekran NIE dostaje z niego Pressed ani Repeat; czeka na puszczenie. Puszczenie
//     jest dostarczane (ze screen = ekran pierwotny), zeby stan tamtego ekranu sie domknal.
//     Dopiero kolejne wcisniecie po puszczeniu naleza do nowego ekranu.
//  3. Czyszczenie kolejki (purgePresses) usuwa WYLACZNIE Pressed i Repeat; zdarzenia Released
//     nigdy nie sa usuwane (inaczej "wcisniety na zawsze"). Kolejnosc pozostalych zachowana.
//  4. Auto-repeat dotyczy WYLACZNIE akcji kierunkowych (isDirectional): po repeatDelayMs
//     od Pressed, potem co repeatRateMs, generowany przez tick(). Pozostale akcje nie
//     powtarzaja sie. Repeat ustaje po Released, utracie fokusu lub zmianie ekranu (zatrzask).
//  5. Utrata fokusu okna: onFocusLost() emituje Released dla wszystkich trzymanych
//     przyciskow. Pad NIE dziala w tle: gdy okno nie ma fokusu, app nie wola consume()
//     dla zrodel pada (stala decyzja produktu, nie ustawienie - kGamepadWorksInBackground).
//  6. Jedno zrodlo fizyczne moze miec wiele akcji (wiazania); jedna akcja wiele zrodel.
//     Akcja jest "trzymana", dopoki trzymane jest dowolne z jej zrodel (Released dopiero po
//     puszczeniu ostatniego).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "vgmplay/core.hpp"

namespace vgmplay::input {

inline constexpr bool kGamepadWorksInBackground = false;

enum class Action : std::uint8_t {
    None = 0,
    // nawigacja (kierunkowe: auto-repeat)
    Up, Down, Left, Right,
    PageUp, PageDown,
    // potwierdzenie / wyjscie
    Confirm, Back, Menu,
    // transport
    PlayPause, Stop, NextTrack, PrevTrack, SeekForward, SeekBack,
    VolumeUp, VolumeDown, ToggleLoop,
    // widok
    NextTab, PrevTab, ToggleTrackerView,
    // kanaly
    ToggleMute, ToggleSolo,
    // inne
    OpenFile, Quit,
    // [dodatek E3] akcje, ktorych UI juz uzywa (kolejnosc = wartosci; dopisywac tylko tutaj).
    ToggleLang, ToggleGd3Lang, ToggleDiag,
    UnmuteAll, ClearSolo, ResetMix,
    PatternFaster, PatternSlower,
    Delete, MoveUp, MoveDown,          // Delete: usun wpis; MoveUp/MoveDown: przenies wpis (Ctrl+Gora/Dol)
    Shuffle, ToggleSettings,
    NewPlaylist, RenamePlaylist, DeletePlaylist,
    MuteChannel, SoloChannel,          // argument = numer kanalu; obslugiwane lokalnie (klawisze 1..9,0)
    Count_
};

enum class ScreenId : std::uint8_t {
    Player = 0, Playlists, Library, Settings, FileBrowser, About,
    // [dodatek E3] kontekst panelu i okien nakladkowych w ekranie glownym.
    Tracker, Info, Dialog, Bindings,
    Count_
};

enum class SourceKind : std::uint8_t { Keyboard = 0, GamepadButton, GamepadAxisPositive, GamepadAxisNegative };

// Fizyczne zrodlo. code: kod klawisza (scancode SDL) lub numer przycisku/osi pada
// (wartosci SDL_GamepadButton / SDL_GamepadAxis); interpretacje robi warstwa app.
struct BindingSource {
    SourceKind kind = SourceKind::Keyboard;
    std::int32_t code = 0;
    bool operator==(const BindingSource&) const = default;
};

struct Binding {
    Action action = Action::None;
    BindingSource source;
    bool operator==(const Binding&) const = default;
};

enum class Phase : std::uint8_t { Pressed = 0, Repeat, Released };

struct RawInput {
    BindingSource source;
    bool down = false;              // true = wcisniecie, false = puszczenie
    std::uint64_t timeMs = 0;       // monotoniczny zegar (np. SDL_GetTicks)
};

struct ActionEvent {
    Action action = Action::None;
    Phase phase = Phase::Pressed;
    ScreenId screen = ScreenId::Player;   // ekran-wlasciciel nacisniecia (zasada 1)
    std::uint64_t timeMs = 0;
};

[[nodiscard]] bool isDirectional(Action a) noexcept;   // Up/Down/Left/Right/PageUp/PageDown

// [dodatek E3] Stabilne nazwy do serializacji (JSON: "action": "play_pause", "source": {"kind": "gamepad_button", "code": 0}).
// Nazwy sa czescia formatu pliku - nie zmieniac. Nieznana nazwa -> nullopt (wiazanie pomijane z ostrzezeniem).
// Action::None i Count_ nie maja nazwy (actionName zwraca ""); MuteChannel/SoloChannel sa lokalne i nie
// podlegaja przypisaniom (actionRebindable() == false).
[[nodiscard]] std::string_view actionName(Action a) noexcept;
[[nodiscard]] std::optional<Action> actionFromName(std::string_view name) noexcept;
[[nodiscard]] std::string_view sourceKindName(SourceKind k) noexcept;
[[nodiscard]] std::optional<SourceKind> sourceKindFromName(std::string_view name) noexcept;
[[nodiscard]] bool actionRebindable(Action a) noexcept;

// [dodatek E3] Domyslne wiazania (klawiatura + pad). Kody klawiszy = SDL_Scancode, pada = SDL_GamepadButton /
// SDL_GamepadAxis (SDL3); zgodnosc pilnuje test.
[[nodiscard]] std::vector<Binding> defaultBindings();

// Usuwa z kolejki tylko Pressed i Repeat (zasada 3).
void purgePresses(std::vector<ActionEvent>& queue);

class InputRouter {
public:
    InputRouter();
    ~InputRouter();
    InputRouter(const InputRouter&) = delete;
    InputRouter& operator=(const InputRouter&) = delete;

    void setBindings(std::span<const Binding> bindings);
    void setRepeat(std::uint32_t delayMs, std::uint32_t rateMs);

    // Zmiana aktywnego ekranu; zatrzaskuje trzymane przyciski (zasada 2).
    void setContext(ScreenId screen);
    [[nodiscard]] ScreenId context() const noexcept;

    // Przetwarza jedno surowe zdarzenie; dopisuje ActionEvent-y do `out` (bez czyszczenia out).
    void consume(const RawInput& in, std::vector<ActionEvent>& out);
    // Auto-repeat; wolac raz na klatke UI.
    void tick(std::uint64_t nowMs, std::vector<ActionEvent>& out);
    // Zasada 5.
    void onFocusLost(std::uint64_t nowMs, std::vector<ActionEvent>& out);

    // [dodatek E3] true, gdy jakis przycisk jest trzymany (petla UI nie usypia, dopoki trwa repeat/zatrzask).
    [[nodiscard]] bool anyHeld() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace vgmplay::input
