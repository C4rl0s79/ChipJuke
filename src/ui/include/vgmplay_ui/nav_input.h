// Prosta nawigacja klawiatura + pad dla makiety (prototyp).
// Docelowo zastapi ja InputRouter w src/input.
//
// Zasady:
//  - nacisniecie nalezy do panelu, w ktorym je rozpoczeto; po zmianie fokusu wcisniety
//    klawisz/przycisk jest "stary" (nie generuje akcji) az do puszczenia,
//  - zdarzenia puszczenia nigdy nie sa gubione (zawsze usuwaja stan),
//  - auto-repeat kierunkow: 350 ms, potem co 60 ms (wlasny, nie systemowy),
//  - pad nie steruje aplikacja, gdy okno nie ma fokusu; przycisk Guide jest ignorowany.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vgmplay::ui {

enum class Panel { Transport, Playlist, Tracker, Count };

enum class Action {
    Up, Down, Left, Right, PageUp, PageDown,
    Accept, Back, TogglePlay, ToggleLang, ToggleGd3Lang, ToggleDiag,
    // Widok kanalow (panel Tracker): MUTE/SOLO aktywnego kanalu, ogolne wylaczniki, zmiana widoku.
    ToggleMute, ToggleSolo, UnmuteAll, ClearSolo, ResetMix,
    ToggleChannelsView,           // F4 / Select: pasek oscyloskopow maly -> duzy -> ukryty
    PatternFaster, PatternSlower, // +/-: krotszy / dluzszy wiersz wzorca (rowTicks)
    MuteChannel, SoloChannel   // arg = numer kanalu 0..9 (klawisze 1..9,0)
};

struct NavEvent {
    Action action;
    bool repeat;
    int arg = 0;
};

class NavInput {
public:
    void handleEvent(const SDL_Event& e, std::uint64_t nowMs);
    // Generuje powtorzenia trzymanych kierunkow; wolac raz na ramke.
    void update(std::uint64_t nowMs);

    void setWindowFocused(bool focused);
    void setFocus(Panel p);
    Panel focus() const { return focus_; }

    // Akcje do obsluzenia w tej ramce (dla biezacego panelu fokusu).
    std::vector<NavEvent>& events() { return events_; }
    bool anyHeld() const { return !held_.empty(); }

    static constexpr std::uint64_t kRepeatDelayMs = 350;
    static constexpr std::uint64_t kRepeatRateMs = 60;

private:
    struct Held {
        Action action;
        int arg;
        bool repeatable;
        bool stale; // nacisniete przed zmiana fokusu - czeka na puszczenie
        std::uint64_t nextRepeat;
    };

    void press(std::uint64_t key, Action a, std::uint64_t now, int arg = 0);
    void release(std::uint64_t key);
    void cycleFocus(int dir);
    void releaseAllForGamepad(SDL_JoystickID id);
    void handleStick(SDL_JoystickID id, Uint8 axis, Sint16 value, std::uint64_t now);

    Panel focus_ = Panel::Transport;
    bool windowFocused_ = true;
    std::unordered_map<std::uint64_t, Held> held_;
    std::unordered_map<std::uint64_t, int> stick_; // (id,axis) -> -1/0/+1
    std::vector<NavEvent> events_;
};

} // namespace vgmplay::ui
