#include "vgmplay_ui/nav_input.h"

#include "vgmplay_input/bindings_edit.hpp"

#include <cstdlib>

namespace vgmplay::ui {
namespace {

using input::ScreenId;
using input::SourceKind;

constexpr int kOnThreshold = 16000;   // histereza osi: 16000 wlacza, 9000 wylacza
constexpr int kOffThreshold = 9000;

std::uint64_t padKey(SDL_JoystickID id, SourceKind kind, std::int32_t code)
{
    return (static_cast<std::uint64_t>(kind) << 56) | (static_cast<std::uint64_t>(id) << 16) |
           static_cast<std::uint64_t>(code & 0xFFFF);
}

SDL_JoystickID idOfKey(std::uint64_t k) { return static_cast<SDL_JoystickID>((k >> 16) & 0xFFFFFFFFull); }
std::int32_t codeOfKey(std::uint64_t k) { return static_cast<std::int32_t>(k & 0xFFFF); }
SourceKind kindOfKey(std::uint64_t k) { return static_cast<SourceKind>(k >> 56); }

bool isModifierKey(SDL_Scancode sc) { return sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI; }

} // namespace

NavInput::NavInput()
{
    router_.setBindings(input::defaultBindings());
    router_.setRepeat(kRepeatDelayMs, kRepeatRateMs);
    updateContext();
}

input::ScreenId NavInput::screenOf(Panel p)
{
    switch (p) {
    case Panel::Playlist: return ScreenId::Playlists;
    case Panel::Info: return ScreenId::Info;
    case Panel::Tracker: return ScreenId::Tracker;
    default: return ScreenId::Player;
    }
}

void NavInput::updateContext() { router_.setContext(overlay_ ? *overlay_ : screenOf(focus_)); }

void NavInput::setBindings(std::span<const input::Binding> bindings)
{
    const std::vector<input::Binding> eff = input::effectiveBindings(bindings);
    router_.setBindings(eff);
}

void NavInput::setRepeat(std::uint32_t delayMs, std::uint32_t rateMs) { router_.setRepeat(delayMs, rateMs); }

void NavInput::setWindowFocused(bool focused, std::uint64_t nowMs)
{
    windowFocused_ = focused;
    // Zmiana fokusu okna: puszczenia moga nie dotrzec - zasada 5 (Released dla wszystkich trzymanych).
    scratch_.clear();
    router_.onFocusLost(nowMs, scratch_);
    scratch_.clear();
    padDown_.clear();
    axis_.clear();
    events_.clear();
    capBHeldSince_ = 0;
}

void NavInput::setFocus(Panel p)
{
    if (p == focus_) return;
    focus_ = p;
    updateContext();
}

void NavInput::setOverlay(std::optional<input::ScreenId> overlay)
{
    if (overlay == overlay_) return;
    overlay_ = overlay;
    updateContext();
}

void NavInput::cycleFocus(int dir)
{
    const int n = static_cast<int>(Panel::Count);
    setFocus(static_cast<Panel>((static_cast<int>(focus_) + dir + n) % n));
}

void NavInput::pushLocal(Action a, bool repeat, int arg)
{
    events_.push_back({a, repeat, arg, overlay_ ? *overlay_ : screenOf(focus_)});
}

void NavInput::dispatch(const std::vector<input::ActionEvent>& evs)
{
    for (const input::ActionEvent& ev : evs) {
        if (ev.phase == input::Phase::Released) continue;   // UI nie potrzebuje puszczen; router je dostarcza
        if (ev.action == Action::NextTab || ev.action == Action::PrevTab) {
            if (overlay_ && *overlay_ == ScreenId::FileBrowser) {   // [E4] LB/RB = strona listy w przegladarce plikow
                if (ev.phase == input::Phase::Pressed)
                    events_.push_back({ev.action == Action::NextTab ? Action::PageDown : Action::PageUp, false, 0, ev.screen});
                continue;
            }
            if (!overlay_ && ev.phase == input::Phase::Pressed) cycleFocus(ev.action == Action::NextTab ? 1 : -1);
            continue;
        }
        events_.push_back({ev.action, ev.phase == input::Phase::Repeat, 0, ev.screen});
    }
}

void NavInput::feed(SourceKind kind, std::int32_t code, bool down, std::uint64_t now)
{
    scratch_.clear();
    router_.consume({{kind, code}, down, now}, scratch_);
    dispatch(scratch_);
    scratch_.clear();
}

void NavInput::feedPad(SDL_JoystickID id, SourceKind kind, std::int32_t code, bool down, std::uint64_t now)
{
    const std::uint64_t key = padKey(id, kind, code);
    bool other = false;   // to samo zrodlo trzymane na innym padzie
    for (std::uint64_t k : padDown_)
        if (k != key && kindOfKey(k) == kind && codeOfKey(k) == code) other = true;
    if (down) {
        if (!padDown_.insert(key).second) return;
        lastPad_ = true;
        if (!other) feed(kind, code, true, now);
    } else {
        if (padDown_.erase(key) == 0) return;
        if (!other) feed(kind, code, false, now);
    }
}

void NavInput::beginCapture(CaptureKind kind, std::uint64_t nowMs)
{
    capState_ = CaptureState::Waiting;
    capKind_ = kind;
    capStart_ = nowMs;
    capBHeldSince_ = 0;
    capResult_ = {};
}

void NavInput::endCapture()
{
    capState_ = CaptureState::Idle;
    capBHeldSince_ = 0;
}

void NavInput::finishCapture(CaptureState s, input::BindingSource src)
{
    capState_ = s;
    capResult_ = src;
    capBHeldSince_ = 0;
}

std::uint64_t NavInput::captureRemainingMs(std::uint64_t nowMs) const
{
    if (capState_ != CaptureState::Waiting) return 0;
    const std::uint64_t used = nowMs > capStart_ ? nowMs - capStart_ : 0;
    return used >= kCaptureTimeoutMs ? 0 : kCaptureTimeoutMs - used;
}

void NavInput::removePad(SDL_JoystickID id, std::uint64_t now)
{
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k : padDown_)
        if (idOfKey(k) == id) keys.push_back(k);
    for (std::uint64_t k : keys) feedPad(id, kindOfKey(k), codeOfKey(k), false, now);
    for (auto it = axis_.begin(); it != axis_.end();) {
        if (idOfKey(it->first) == id) it = axis_.erase(it);
        else ++it;
    }
    pads_.erase(id);
}

void NavInput::handleAxis(SDL_JoystickID id, Uint8 axis, Sint16 value, std::uint64_t now)
{
    const std::uint64_t ak = padKey(id, SourceKind::GamepadAxisPositive, axis);   // klucz stanu osi
    int& state = axis_[ak];
    int next = state;
    const int mag = std::abs(static_cast<int>(value));
    if (value > 0 && mag > kOnThreshold) next = 1;
    else if (value < 0 && mag > kOnThreshold) next = -1;
    else if (mag < kOffThreshold) next = 0;
    if (next == state) return;
    const int old = state;
    state = next;
    if (old != 0)
        feedPad(id, old > 0 ? SourceKind::GamepadAxisPositive : SourceKind::GamepadAxisNegative, axis, false, now);
    if (next == 0) return;
    const SourceKind kind = next > 0 ? SourceKind::GamepadAxisPositive : SourceKind::GamepadAxisNegative;
    if (capState_ == CaptureState::Waiting) {
        if (capKind_ == CaptureKind::Gamepad) finishCapture(CaptureState::Done, {kind, axis});
        return;
    }
    feedPad(id, kind, axis, true, now);
}

void NavInput::handleEvent(const SDL_Event& e, std::uint64_t now)
{
    lastNow_ = now;
    switch (e.type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED: setWindowFocused(true, now); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: setWindowFocused(false, now); break;

    case SDL_EVENT_KEY_DOWN: {
        if (!windowFocused_) break;
        const SDL_Scancode sc = e.key.scancode;
        if (capState_ == CaptureState::Waiting) {
            if (e.key.repeat || isModifierKey(sc)) break;
            if (sc == SDL_SCANCODE_ESCAPE) finishCapture(CaptureState::Cancelled);
            else if (capKind_ == CaptureKind::Keyboard)
                finishCapture(CaptureState::Done, {SourceKind::Keyboard, static_cast<std::int32_t>(sc)});
            break;
        }
        lastPad_ = false;
        const bool shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
        if (ctrl) {   // Ctrl+Gora/Dol przenosi wpis (z repeatem systemowym); inne Ctrl+... naleza do aplikacji
            if (sc == SDL_SCANCODE_UP) pushLocal(Action::MoveUp, e.key.repeat);
            else if (sc == SDL_SCANCODE_DOWN) pushLocal(Action::MoveDown, e.key.repeat);
            break;
        }
        if (e.key.repeat) break;   // powtorzenia kierunkowych robi router
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {   // 1..9, 0 -> kanaly 0..9
            pushLocal(shift ? Action::SoloChannel : Action::MuteChannel, false, static_cast<int>(sc - SDL_SCANCODE_1));
            break;
        }
        if (sc == SDL_SCANCODE_TAB && shift) {
            if (!overlay_) cycleFocus(-1);
            break;
        }
        if (sc == SDL_SCANCODE_DELETE && shift) { pushLocal(Action::DeletePlaylist, false); break; }
        feed(SourceKind::Keyboard, static_cast<std::int32_t>(sc), true, now);
        break;
    }
    case SDL_EVENT_KEY_UP:   // puszczenie zawsze, bez warunku na fokus
        feed(SourceKind::Keyboard, static_cast<std::int32_t>(e.key.scancode), false, now);
        break;

    case SDL_EVENT_GAMEPAD_ADDED:   // hotplug
        SDL_OpenGamepad(e.gdevice.which);
        pads_.insert(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        removePad(e.gdevice.which, now);
        if (SDL_Gamepad* g = SDL_GetGamepadFromID(e.gdevice.which)) SDL_CloseGamepad(g);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (!windowFocused_) break;   // pad nie steruje aplikacja bez fokusu okna
        const Uint8 b = e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_GUIDE) break;
        if (capState_ == CaptureState::Waiting) {
            if (capKind_ != CaptureKind::Gamepad) break;
            if (b == SDL_GAMEPAD_BUTTON_EAST) { capBHeldSince_ = now; break; }   // B: krotko = przypisz, dlugo = anuluj
            finishCapture(CaptureState::Done, {SourceKind::GamepadButton, b});
            break;
        }
        feedPad(e.gbutton.which, SourceKind::GamepadButton, b, true, now);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (capState_ == CaptureState::Waiting && capBHeldSince_ != 0 && e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
            capBHeldSince_ = 0;
            finishCapture(CaptureState::Done, {SourceKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST});
            break;
        }
        feedPad(e.gbutton.which, SourceKind::GamepadButton, e.gbutton.button, false, now);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (windowFocused_) handleAxis(e.gaxis.which, e.gaxis.axis, e.gaxis.value, now);
        break;
    default: break;
    }
}

void NavInput::update(std::uint64_t now)
{
    lastNow_ = now;
    if (capState_ == CaptureState::Waiting) {
        const std::uint64_t held = capBHeldSince_ != 0 && now > capBHeldSince_ ? now - capBHeldSince_ : 0;
        const std::uint64_t used = now > capStart_ ? now - capStart_ : 0;
        if (held >= kCaptureCancelHoldMs) finishCapture(CaptureState::Cancelled);
        else if (used >= kCaptureTimeoutMs) finishCapture(CaptureState::TimedOut);
    }
    if (!windowFocused_) return;
    scratch_.clear();
    router_.tick(now, scratch_);
    dispatch(scratch_);
    scratch_.clear();
}

} // namespace vgmplay::ui
