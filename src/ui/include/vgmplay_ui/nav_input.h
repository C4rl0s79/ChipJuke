// Warstwa SDL -> InputRouter (src/input): tlumaczy zdarzenia SDL na RawInput, zamienia ActionEvent
// na NavEvent dla biezacego ekranu i trzyma fokus panelu (Panel -> ScreenId).
//
// Zasady (kontrakt: include/vgmplay/input.hpp):
//  - nacisniecie nalezy do ekranu, na ktorym je rozpoczeto; zmiana panelu/okna nakladkowego zatrzaskuje
//    trzymane przyciski (nowy ekran czeka na puszczenie),
//  - zdarzenia puszczenia nigdy nie sa gubione,
//  - auto-repeat tylko kierunkowych (opoznienie/tempo z setRepeat),
//  - pad nie steruje aplikacja, gdy okno nie ma fokusu (input::kGamepadWorksInBackground == false).
// Skroty z modyfikatorami (Ctrl+Gora/Dol, Shift+Tab, Shift+Delete, cyfry kanalow) sa obslugiwane tu
// lokalnie - nie podlegaja przypisaniom.
#pragma once

#include "vgmplay/input.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vgmplay::ui {

// Kolejnosc = kolejnosc przechodzenia Tab / LB / RB.
enum class Panel { Transport, Playlist, Info, Tracker, Count };

using Action = input::Action;

struct NavEvent {
    Action action;
    bool repeat;
    int arg = 0;
    input::ScreenId screen = input::ScreenId::Player;   // ekran-wlasciciel nacisniecia
};

// Przechwytywanie jednego zrodla do ekranu przypisan.
enum class CaptureKind { Keyboard, Gamepad };
enum class CaptureState { Idle, Waiting, Done, Cancelled, TimedOut };

class NavInput {
public:
    NavInput();

    void handleEvent(const SDL_Event& e, std::uint64_t nowMs);
    // Generuje powtorzenia trzymanych kierunkow i obsluguje limity czasu przechwytywania; raz na ramke.
    void update(std::uint64_t nowMs);

    void setWindowFocused(bool focused, std::uint64_t nowMs = 0);
    [[nodiscard]] bool windowFocused() const { return windowFocused_; }
    void setFocus(Panel p);
    [[nodiscard]] Panel focus() const { return focus_; }
    // Okno nakladkowe (ustawienia, dialog, przypisania): wlasny kontekst routera; nullopt = panel z fokusem.
    void setOverlay(std::optional<input::ScreenId> overlay);
    [[nodiscard]] bool overlayActive() const { return overlay_.has_value(); }

    void setBindings(std::span<const input::Binding> bindings);   // pusty = domyslne
    void setRepeat(std::uint32_t delayMs, std::uint32_t rateMs);

    // Akcje do obsluzenia w tej ramce.
    std::vector<NavEvent>& events() { return events_; }
    [[nodiscard]] bool anyHeld() const { return router_.anyHeld(); }
    // true, gdy ostatnim uzytym wejsciem byl pad (steruje paskiem podpowiedzi).
    [[nodiscard]] bool lastInputWasPad() const { return lastPad_; }
    [[nodiscard]] bool gamepadConnected() const { return !pads_.empty(); }

    // --- przechwytywanie (ekran przypisan): router nie dostaje zdarzen, dopoki stan == Waiting ---
    static constexpr std::uint64_t kCaptureTimeoutMs = 6000;
    static constexpr std::uint64_t kCaptureCancelHoldMs = 1000;   // przytrzymane B (pad)
    void beginCapture(CaptureKind kind, std::uint64_t nowMs);
    void endCapture();   // wyzeruj stan (Idle)
    [[nodiscard]] CaptureState captureState() const { return capState_; }
    [[nodiscard]] CaptureKind captureKind() const { return capKind_; }
    [[nodiscard]] input::BindingSource captureResult() const { return capResult_; }
    [[nodiscard]] std::uint64_t captureRemainingMs(std::uint64_t nowMs) const;

    // Zgodne z ustawieniami domyslnymi (settings::InputSettings).
    static constexpr std::uint64_t kRepeatDelayMs = 400;
    static constexpr std::uint64_t kRepeatRateMs = 60;

    // Panel -> kontekst routera.
    static input::ScreenId screenOf(Panel p);

private:
    void feed(input::SourceKind kind, std::int32_t code, bool down, std::uint64_t now);
    void feedPad(SDL_JoystickID id, input::SourceKind kind, std::int32_t code, bool down, std::uint64_t now);
    void dispatch(const std::vector<input::ActionEvent>& evs);
    void pushLocal(Action a, bool repeat, int arg = 0);
    void cycleFocus(int dir);
    void updateContext();
    void handleAxis(SDL_JoystickID id, Uint8 axis, Sint16 value, std::uint64_t now);
    void removePad(SDL_JoystickID id, std::uint64_t now);
    void finishCapture(CaptureState s, input::BindingSource src = {});

    input::InputRouter router_;
    Panel focus_ = Panel::Transport;
    std::optional<input::ScreenId> overlay_;
    bool windowFocused_ = true;
    bool lastPad_ = false;
    std::vector<NavEvent> events_;
    std::vector<input::ActionEvent> scratch_;

    std::unordered_set<std::uint64_t> padDown_;      // (id, rodzaj, kod) trzymanych zrodel pada
    std::unordered_map<std::uint64_t, int> axis_;    // (id, os) -> -1/0/+1 (histereza)
    std::unordered_set<SDL_JoystickID> pads_;

    CaptureState capState_ = CaptureState::Idle;
    CaptureKind capKind_ = CaptureKind::Keyboard;
    input::BindingSource capResult_;
    std::uint64_t capStart_ = 0;
    std::uint64_t capBHeldSince_ = 0;                // 0 = B (pad) nie jest trzymane
    std::uint64_t lastNow_ = 0;
};

} // namespace vgmplay::ui
